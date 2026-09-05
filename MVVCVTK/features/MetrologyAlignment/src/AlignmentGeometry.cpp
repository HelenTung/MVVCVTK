#include "AlignmentGeometry.h"
#include "AlignmentMath.h"
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
bool Positive(double x) {
    return std::isfinite(x) && x > 0;
}
bool Direction(const AlignmentPoint &p) {
    const auto v = Vector(p);
    return Finite(v) && std::abs(cv::norm(v) - 1) < 1e-8;
}
const MeshAttribute *Attribute(const SurfaceMeshPayload &mesh, const char *name,
                               std::size_t components) {
    const MeshAttribute *found = nullptr;
    for (const auto &a : mesh.GetPointAttributes())
        if (a.name == name) {
            Require(!found && a.componentCount == components,
                    "Invalid measurement attribute schema.");
            found = &a;
        }
    return found;
}
struct Shape final {
    Vec center{};
    Vec normal{0, 0, 1};
    double radius = 1;
};
std::vector<double> Residuals(const Shape &shape, AlignmentGeometryKind kind,
                              const std::vector<Vec> &points) {
    std::vector<double> r;
    r.reserve(points.size() * 2);
    for (const auto &p : points) {
        const auto q = p - shape.center;
        const double axial = q.dot(shape.normal);
        r.push_back((kind == AlignmentGeometryKind::Sphere ? cv::norm(q)
                                                           : cv::norm(q - axial * shape.normal)) -
                    shape.radius);
        if (kind == AlignmentGeometryKind::Circle)
            r.push_back(axial);
    }
    return r;
}
Shape Shift(Shape s, AlignmentGeometryKind kind, const cv::Mat &dx) {
    if (kind == AlignmentGeometryKind::Sphere) {
        s.center += Vec(dx.at<double>(0), dx.at<double>(1), dx.at<double>(2));
        s.radius += dx.at<double>(3);
        return s;
    }
    const Vec a = Tangent(s.normal), b = s.normal.cross(a);
    if (kind == AlignmentGeometryKind::Cylinder) {
        s.center += a * dx.at<double>(0) + b * dx.at<double>(1);
        s.normal = Unit(s.normal + a * dx.at<double>(2) + b * dx.at<double>(3));
        s.radius += dx.at<double>(4);
    } else {
        s.center += Vec(dx.at<double>(0), dx.at<double>(1), dx.at<double>(2));
        s.normal = Unit(s.normal + a * dx.at<double>(3) + b * dx.at<double>(4));
        s.radius += dx.at<double>(5);
    }
    return s;
}
double Cost(const std::vector<double> &r) {
    return std::inner_product(r.begin(), r.end(), r.begin(), 0.0);
}
Shape FitRound(const AlignmentWork &work, AlignmentGeometryKind kind,
               const std::vector<Vec> &points, Shape shape) {
    const bool sphere = kind == AlignmentGeometryKind::Sphere;
    const int columns = sphere ? 4 : 3;
    const auto a = Tangent(shape.normal), b = shape.normal.cross(a);
    cv::Mat design(static_cast<int>(points.size()), columns, CV_64F),
        values(static_cast<int>(points.size()), 1, CV_64F);
    for (int i = 0; i < design.rows; ++i) {
        const auto &p = points[static_cast<std::size_t>(i)];
        if (sphere) {
            for (int j = 0; j < 3; ++j)
                design.at<double>(i, j) = 2 * p[j];
            design.at<double>(i, 3) = 1;
            values.at<double>(i) = p.dot(p);
        } else {
            const double u = p.dot(a), v = p.dot(b);
            design.at<double>(i, 0) = 2 * u;
            design.at<double>(i, 1) = 2 * v;
            design.at<double>(i, 2) = 1;
            values.at<double>(i) = u * u + v * v;
        }
    }
    auto rank = Decompose(design, work.recipe.rankTolerance);
    Require(rank.rank == columns, "Degenerate round geometry.");
    const cv::Mat answer = rank.inverse * values;
    shape.center = sphere ? Vec(answer.at<double>(0), answer.at<double>(1), answer.at<double>(2))
                          : a * answer.at<double>(0) + b * answer.at<double>(1);
    shape.radius =
        std::sqrt(std::max(0.0, answer.at<double>(columns - 1) + shape.center.dot(shape.center)));
    Require(Positive(shape.radius), "Degenerate radius.");
    const int count = sphere ? 4 : (kind == AlignmentGeometryKind::Cylinder ? 5 : 6);
    bool converged = false;
    for (std::size_t iteration = 0; iteration < work.recipe.iterationLimit; ++iteration) {
        if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
            throw std::runtime_error("Cancelled geometry fit.");
        const auto r = Residuals(shape, kind, points);
        cv::Mat jac(static_cast<int>(r.size()), count, CV_64F);
        // 解析 Jacobian 避免短圆弧在最优点附近的差分消减误差；参数均为规范化长度。
        const auto tangent = Tangent(shape.normal), bitangent = shape.normal.cross(tangent);
        jac.setTo(0);
        for (std::size_t i = 0; i < points.size(); ++i) {
            if ((i % 256) == 0 &&
                AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
                throw std::runtime_error("Cancelled shape Jacobian.");
            const auto q = points[i] - shape.center;
            const double axial = q.dot(shape.normal);
            const auto radial = q - axial * shape.normal;
            const double distance = sphere ? cv::norm(q) : cv::norm(radial);
            Require(distance > 1e-15, "Point lies on singular shape center/axis.");
            const auto direction = (sphere ? q : radial) / distance;
            const int row = static_cast<int>(i) * (kind == AlignmentGeometryKind::Circle ? 2 : 1);
            if (kind == AlignmentGeometryKind::Cylinder) {
                jac.at<double>(row, 0) = -direction.dot(tangent);
                jac.at<double>(row, 1) = -direction.dot(bitangent);
                jac.at<double>(row, 2) = -axial * direction.dot(tangent);
                jac.at<double>(row, 3) = -axial * direction.dot(bitangent);
                jac.at<double>(row, 4) = -1;
            } else {
                for (int j = 0; j < 3; ++j)
                    jac.at<double>(row, j) = -direction[j];
                if (sphere)
                    jac.at<double>(row, 3) = -1;
                else {
                    jac.at<double>(row, 3) = -axial * direction.dot(tangent);
                    jac.at<double>(row, 4) = -axial * direction.dot(bitangent);
                    jac.at<double>(row, 5) = -1;
                    for (int j = 0; j < 3; ++j)
                        jac.at<double>(row + 1, j) = -shape.normal[j];
                    jac.at<double>(row + 1, 3) = q.dot(tangent);
                    jac.at<double>(row + 1, 4) = q.dot(bitangent);
                }
            }
        }
        rank = Decompose(jac, work.recipe.rankTolerance);
        Require(rank.rank == count && rank.condition <= work.recipe.conditionLimit,
                "Degenerate shape coverage.");
        cv::Mat residual(static_cast<int>(r.size()), 1, CV_64F);
        for (int i = 0; i < residual.rows; ++i)
            residual.at<double>(i) = r[static_cast<std::size_t>(i)];
        const cv::Mat dx = -rank.inverse * residual;
        if (cv::norm(dx) < work.recipe.solveTolerance / work.recipe.lengthScale) {
            converged = true;
            break;
        }
        bool improved = false;
        for (double fraction = 1; fraction >= 1.0 / 1024; fraction *= 0.5) {
            const auto candidate = Shift(shape, kind, dx * fraction);
            if (candidate.radius > 0 && Cost(Residuals(candidate, kind, points)) < Cost(r)) {
                shape = candidate;
                improved = true;
                break;
            }
        }
        if (!improved) {
            const double predicted = cv::norm(jac * dx);
            const double epsilon = std::numeric_limits<double>::epsilon();
            // 当预测的目标下降量已经低于浮点目标值的分辨率时，不能无限迭代。
            // 几何质量和条件数仍分别检查，这不放宽外部参考几何的比较阈值。
            converged = cv::norm(jac.t() * residual) <
                            work.recipe.solveTolerance / work.recipe.lengthScale ||
                        predicted * predicted <= 64 * epsilon * std::max(Cost(r), epsilon);
            break;
        }
    }
    Require(converged, "Round geometry did not converge.");
    return shape;
}
} // namespace

bool AlignmentGeometryFit::GetRecipeValid(const AlignmentRecipe &r, const AlignmentConfig &config) {
    if (r.minPairNormalCosine && (!std::isfinite(*r.minPairNormalCosine) ||
                                  *r.minPairNormalCosine < 0 || *r.minPairNormalCosine > 1))
        return false;
    if (r.id.empty() || r.id.size() > 128 || r.targetFrameId.empty() ||
        r.targetFrameId.size() > 128 || static_cast<unsigned>(r.unit) > 2 ||
        static_cast<unsigned>(r.method) > 3 || r.geometries.size() > 64 ||
        r.constraints.size() > config.constraintLimit ||
        r.fitPairs.size() > config.constraintLimit || r.datumCount > 3 ||
        r.datumCount > r.geometries.size() || r.iterationLimit == 0 || r.iterationLimit > 1000 ||
        !Positive(r.lengthScale) || !Positive(r.rankTolerance) || r.rankTolerance >= 0.01 ||
        !Positive(r.conditionLimit) || r.conditionLimit <= 1 || !Positive(r.solveTolerance) ||
        !Positive(r.maxAlignmentRms) || !Positive(r.maxPairDistance) ||
        !Positive(r.huberDistance) || !Positive(r.minPairCoverage) || r.minPairCoverage > 1 ||
        !std::isfinite(r.minSupportRatio) || r.minSupportRatio < 0 || r.minSupportRatio > 1 ||
        !Positive(r.maxLocalizationSigma) || !Positive(r.maxSurfaceFitResidual) ||
        !Finite(Vector(r.datumOffsets)))
        return false;
    if (!GetDataRevisionRefValid(r.nominalData))
        return false;
    if (r.method == AlignmentMethod::SequentialPlanes &&
        (r.datumCount == 0 || r.geometries.size() != r.datumCount))
        return false;
    if (r.method == AlignmentMethod::PlaneTwoHoles && r.geometries.size() != 3)
        return false;
    if (r.method == AlignmentMethod::Rps && r.constraints.empty())
        return false;
    if (r.method == AlignmentMethod::ConstrainedBestFit &&
        (r.fitPairs.empty() || !GetDataRevisionRefValid(r.exactMesh)))
        return false;
    if (r.method != AlignmentMethod::ConstrainedBestFit && !r.fitPairs.empty())
        return false;
    std::set<std::string> names;
    std::size_t selected = 0;
    for (std::size_t geometryIndex = 0; geometryIndex < r.geometries.size(); ++geometryIndex) {
        const auto &g = r.geometries[geometryIndex];
        if (g.id.empty() || !names.insert(g.id).second || static_cast<unsigned>(g.kind) > 5 ||
            static_cast<unsigned>(g.association) > 2 || !Direction(g.targetDirection) ||
            !Positive(g.maxFitRms) || !Positive(g.minCoverage) || g.minCoverage > 1 ||
            !Positive(g.minDirectionCosine) || g.minDirectionCosine > 1)
            return false;
        if (g.association == AlignmentAssociation::SequentialLeastSquares &&
            (g.kind != AlignmentGeometryKind::Plane || geometryIndex >= r.datumCount ||
             (r.method != AlignmentMethod::SequentialPlanes &&
              r.method != AlignmentMethod::ConstrainedBestFit)))
            return false;
        const auto &region = g.region;
        if (region.vertexIds.empty() == !region.targetBounds.has_value())
            return false;
        if (!region.vertexIds.empty() && !GetDataRevisionRefValid(region.pinnedMesh))
            return false;
        if (region.vertexIds.size() > config.pointLimit - selected)
            return false;
        selected += region.vertexIds.size();
        if (region.targetBounds)
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite((*region.targetBounds)[2 * i]) ||
                    !std::isfinite((*region.targetBounds)[2 * i + 1]) ||
                    (*region.targetBounds)[2 * i] > (*region.targetBounds)[2 * i + 1])
                    return false;
        if (g.radiusRange && (!Positive((*g.radiusRange)[0]) || !Positive((*g.radiusRange)[1]) ||
                              (*g.radiusRange)[0] > (*g.radiusRange)[1]))
            return false;
    }
    for (const auto &c : r.constraints)
        if (c.geometryIndex >= r.geometries.size() || !Finite(Vector(c.nominalPoint)) ||
            !Direction(c.targetDirection) || static_cast<unsigned>(c.kind) > 2 || c.priority > 63 ||
            !Positive(c.weight) || c.weight > 1e12 || !Positive(c.tolerance) ||
            (r.method == AlignmentMethod::ConstrainedBestFit &&
             c.kind == AlignmentConstraintKind::Soft && c.priority >= 63) ||
            (c.sectionPlane &&
             (*c.sectionPlane >= r.geometries.size() ||
              r.geometries[*c.sectionPlane].kind != AlignmentGeometryKind::Plane)))
            return false;
    for (const auto &p : r.fitPairs)
        if (!Finite(Vector(p.nominalPoint)) || !Positive(p.weight) || p.weight > 1e12 ||
            (p.targetNormal && !Direction(*p.targetNormal)))
            return false;
    return true;
}

AlignmentStatus AlignmentGeometryFit::GetWorkStatus(const AlignmentWork &work) {
    if (work.cancelled && work.cancelled->load(std::memory_order_relaxed))
        return AlignmentStatus::Cancelled;
    if (std::chrono::steady_clock::now() >= work.deadline)
        return AlignmentStatus::DeadlineExceeded;
    return AlignmentStatus::FullyDetermined;
}

bool AlignmentGeometryFit::GetPointUsable(const AlignmentWork &work, std::size_t index) {
    Require(work.mesh && index < work.mesh->GetVertices().size() / 3,
            "Invalid measurement point index.");
    const auto count = work.mesh->GetVertices().size() / 3;
    const auto attribute = [&](const char *name, std::size_t components) {
        const auto *value = Attribute(*work.mesh, name, components);
        Require(!value || value->values.size() == count * components,
                "Invalid measurement attribute length.");
        return value;
    };
    const auto *valid = attribute("measurement.valid", 1);
    if (valid) {
        Require(valid->values[index] == 0 || valid->values[index] == 1,
                "Invalid measurement validity.");
        if (valid->values[index] == 0)
            return false;
    }
    if (!work.recipe.requiresMeasurementQuality)
        return true;
    const auto *support = attribute("measurement.support-ratio", 1);
    const auto *sigma = attribute("measurement.localization-sigma", 1);
    const auto *residual = attribute("measurement.fit-residual", 1);
    const auto *normal = attribute("measurement.normal", 3);
    Require(valid && support && sigma && residual && normal, "Measurement quality is unavailable.");
    const double supportValue = support->values[index], sigmaValue = sigma->values[index],
                 residualValue = residual->values[index];
    const Vec direction(normal->values[3 * index], normal->values[3 * index + 1],
                        normal->values[3 * index + 2]);
    return std::isfinite(supportValue) && supportValue >= work.recipe.minSupportRatio &&
           supportValue <= 1 && std::isfinite(sigmaValue) && sigmaValue >= 0 &&
           sigmaValue <= work.recipe.maxLocalizationSigma && std::isfinite(residualValue) &&
           residualValue >= 0 && residualValue <= work.recipe.maxSurfaceFitResidual &&
           Finite(direction) && std::abs(cv::norm(direction) - 1) <= 1e-6;
}

AlignmentSamples AlignmentGeometryFit::BuildSamples(const AlignmentWork &work,
                                                    const AlignmentRegion &region,
                                                    const AlignmentMatrix &pose) {
    Require(work.mesh && work.mesh->GetVertices().size() % 3 == 0, "Invalid mesh.");
    Require(region.vertexIds.empty() || region.pinnedMesh == work.input.mesh,
            "Selection belongs to another mesh revision.");
    const auto &vertices = work.mesh->GetVertices();
    const std::size_t n = vertices.size() / 3;
    AlignmentSamples result;
    std::set<std::uint64_t> seen;
    const auto add = [&](std::size_t index) {
        Require(index < n, "Selection index is out of bounds.");
        const Vec source(vertices[3 * index], vertices[3 * index + 1], vertices[3 * index + 2]);
        Require(Finite(source), "Non-finite measurement position.");
        if (region.targetBounds) {
            const auto p = Transform(pose, source);
            for (int j = 0; j < 3; ++j)
                if (p[j] < (*region.targetBounds)[2 * j] ||
                    p[j] > (*region.targetBounds)[2 * j + 1])
                    return;
        }
        if (!GetPointUsable(work, index)) {
            ++result.rejected;
            return;
        }
        if (result.points.size() >= work.config.pointLimit)
            throw std::length_error("Selection exceeds point budget.");
        result.points.push_back(Point(source));
    };
    if (!region.vertexIds.empty()) {
        for (const auto id : region.vertexIds) {
            Require(seen.insert(id).second, "Duplicate selection vertex.");
            if (GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
                throw std::runtime_error("Cancelled extraction.");
            add(static_cast<std::size_t>(id));
        }
    } else {
        Require(region.targetBounds.has_value(), "Selection is missing.");
        for (std::size_t i = 0; i < n; ++i) {
            if ((i % 256) == 0 && GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
                throw std::runtime_error("Cancelled extraction.");
            add(i);
        }
    }
    return result;
}

AlignmentGeometry AlignmentGeometryFit::BuildGeometry(
    const AlignmentWork &work, const AlignmentGeometrySpec &spec, const AlignmentSamples &samples,
    const AlignmentMatrix &pose, std::optional<AlignmentPoint> normalConstraint,
    bool isFixedNormal) {
    Require(spec.association != AlignmentAssociation::Contact, "Unsupported contact association.");
    Require(Rigid(pose), "Geometry initialization must be rigid.");
    constexpr std::size_t minimum[]{1, 2, 3, 3, 4, 6};
    const std::size_t required = spec.kind == AlignmentGeometryKind::Plane && normalConstraint
                                     ? (isFixedNormal ? 1U : 2U)
                                     : minimum[static_cast<std::size_t>(spec.kind)];
    Require(samples.points.size() >= required, "Insufficient geometry samples.");
    Require(static_cast<double>(samples.points.size()) /
                    static_cast<double>(samples.points.size() + samples.rejected) >=
                spec.minCoverage,
            "Insufficient valid coverage.");
    Vec center{};
    for (const auto &p : samples.points)
        center += Vector(p);
    center /= static_cast<double>(samples.points.size());
    std::vector<Vec> points;
    points.reserve(samples.points.size());
    cv::Mat scatter = cv::Mat::zeros(3, 3, CV_64F);
    for (const auto &p : samples.points) {
        const auto q = (Vector(p) - center) / work.recipe.lengthScale;
        points.push_back(q);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                scatter.at<double>(i, j) += q[i] * q[j];
    }
    cv::SVD svd(scatter);
    Shape shape;
    shape.normal = Unit(Rotate(Inverse(pose), Vector(spec.targetDirection)));
    const auto row = [&](int index) {
        return Vec(svd.vt.at<double>(index, 0), svd.vt.at<double>(index, 1),
                   svd.vt.at<double>(index, 2));
    };
    if (spec.kind == AlignmentGeometryKind::Plane || spec.kind == AlignmentGeometryKind::Circle) {
        if (!normalConstraint)
            Require(svd.w.at<double>(1) >
                        work.recipe.rankTolerance * std::max(1.0, svd.w.at<double>(0)),
                    "Collinear plane samples.");
        auto normal = row(2);
        if (normalConstraint) {
            const auto n = Unit(Vector(*normalConstraint));
            if (isFixedNormal)
                normal = n;
            else {
                const auto a = Tangent(n), b = n.cross(a);
                cv::Mat covariance = cv::Mat::zeros(2, 2, CV_64F);
                for (const auto &p : points) {
                    const double u = p.dot(a), v = p.dot(b);
                    covariance.at<double>(0, 0) += u * u;
                    covariance.at<double>(0, 1) += u * v;
                    covariance.at<double>(1, 0) += u * v;
                    covariance.at<double>(1, 1) += v * v;
                }
                cv::SVD restricted(covariance);
                Require(restricted.w.at<double>(0) > work.recipe.rankTolerance &&
                            restricted.w.at<double>(0) - restricted.w.at<double>(1) >
                                work.recipe.rankTolerance *
                                    std::max(1.0, restricted.w.at<double>(0)),
                        "Degenerate secondary datum.");
                normal = a * restricted.vt.at<double>(1, 0) + b * restricted.vt.at<double>(1, 1);
            }
        }
        Require(std::abs(normal.dot(shape.normal)) >= spec.minDirectionCosine,
                "Ambiguous datum direction.");
        shape.normal = normal.dot(shape.normal) < 0 ? -normal : normal;
    } else if (spec.kind == AlignmentGeometryKind::Line) {
        Require(svd.w.at<double>(0) > work.recipe.rankTolerance, "Coincident line samples.");
        auto direction = row(0);
        shape.normal = direction.dot(shape.normal) < 0 ? -direction : direction;
    }
    if (spec.kind == AlignmentGeometryKind::Sphere || spec.kind == AlignmentGeometryKind::Circle ||
        spec.kind == AlignmentGeometryKind::Cylinder)
        shape = FitRound(work, spec.kind, points, shape);
    if (spec.kind == AlignmentGeometryKind::Cylinder)
        shape.center -= shape.center.dot(shape.normal) * shape.normal;
    AlignmentGeometry output;
    output.id = spec.id;
    output.kind = spec.kind;
    output.sourceCenter = Point(center + shape.center * work.recipe.lengthScale);
    output.sourceDirection = Point(shape.normal);
    output.radius =
        (spec.kind == AlignmentGeometryKind::Sphere || spec.kind == AlignmentGeometryKind::Circle ||
         spec.kind == AlignmentGeometryKind::Cylinder)
            ? shape.radius * work.recipe.lengthScale
            : 0;
    output.sampleCount = points.size();
    output.rejectedCount = samples.rejected;
    double sum = 0;
    for (const auto &p : points) {
        const auto q = p - shape.center;
        const auto axial = q.dot(shape.normal);
        double distance = 0;
        switch (spec.kind) {
        case AlignmentGeometryKind::Point:
            distance = cv::norm(q);
            break;
        case AlignmentGeometryKind::Line:
            distance = cv::norm(q - axial * shape.normal);
            break;
        case AlignmentGeometryKind::Plane:
            distance = std::abs(axial);
            break;
        case AlignmentGeometryKind::Sphere:
            distance = std::abs(cv::norm(q) - shape.radius);
            break;
        case AlignmentGeometryKind::Circle:
            distance = std::hypot(cv::norm(q - axial * shape.normal) - shape.radius, axial);
            break;
        case AlignmentGeometryKind::Cylinder:
            distance = std::abs(cv::norm(q - axial * shape.normal) - shape.radius);
            break;
        }
        distance *= work.recipe.lengthScale;
        sum += distance * distance;
        output.fitMax = std::max(output.fitMax, distance);
    }
    output.fitRms = std::sqrt(sum / static_cast<double>(points.size()));
    Require(std::isfinite(output.fitRms) && output.fitRms <= spec.maxFitRms,
            "Geometry quality failed.");
    if (spec.radiusRange)
        Require(output.radius >= (*spec.radiusRange)[0] && output.radius <= (*spec.radiusRange)[1],
                "Geometry radius outside recipe range.");
    return output;
}
