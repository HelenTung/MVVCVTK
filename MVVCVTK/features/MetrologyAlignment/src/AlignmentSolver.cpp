#include "AlignmentSolver.h"
#include "AlignmentMath.h"
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace {
using namespace AlignmentMath;
struct Equation final {
    Vec source{}, target{}, direction{};
    AlignmentConstraintKind kind = AlignmentConstraintKind::Hard;
    std::uint32_t priority = 0;
    double weight = 1, tolerance = 1e-6;
    std::string id;
    bool isVector = false;
    bool isUsed = true;
};
void Require(bool valid, const char *message) {
    if (!valid)
        throw std::invalid_argument(message);
}
Vec GeometryPoint(const AlignmentGeometry &geometry,
                  const std::optional<AlignmentGeometry> &section) {
    auto p = Vector(geometry.sourceCenter);
    if (geometry.kind == AlignmentGeometryKind::Cylinder ||
        geometry.kind == AlignmentGeometryKind::Line) {
        Require(section.has_value(), "An infinite axis requires an explicit section plane.");
        const auto n = Vector(section->sourceDirection), v = Vector(geometry.sourceDirection);
        const double denominator = n.dot(v);
        Require(std::abs(denominator) > 1e-8, "Axis is parallel to its section plane.");
        p += v * (n.dot(Vector(section->sourceCenter) - p) / denominator);
    }
    return p;
}
double Residual(const Equation &e, const AlignmentMatrix &pose) {
    return e.direction.dot((e.isVector ? Rotate(pose, e.source) : Transform(pose, e.source)) -
                           e.target);
}
cv::Mat Jacobian(const std::vector<Equation> &equations, const AlignmentMatrix &pose,
                 const std::vector<std::size_t> &indices, double length, const Vec &pivot) {
    cv::Mat result(static_cast<int>(indices.size()), 6, CV_64F);
    for (int i = 0; i < result.rows; ++i) {
        const auto &e = equations[indices[static_cast<std::size_t>(i)]];
        const auto p = e.isVector ? Rotate(pose, e.source) : Transform(pose, e.source) - pivot;
        const auto angular = p.cross(e.direction) / length;
        for (int j = 0; j < 3; ++j) {
            result.at<double>(i, j) = angular[j];
            result.at<double>(i, j + 3) = e.isVector ? 0 : e.direction[j];
        }
    }
    return result;
}
cv::Mat Values(const std::vector<Equation> &equations, const AlignmentMatrix &pose,
               const std::vector<std::size_t> &indices, const std::vector<double> &frozen) {
    cv::Mat result(static_cast<int>(indices.size()), 1, CV_64F);
    for (int i = 0; i < result.rows; ++i) {
        const auto index = indices[static_cast<std::size_t>(i)];
        result.at<double>(i) = Residual(equations[index], pose) - frozen[index];
    }
    return result;
}
Vec Pivot(const std::vector<Equation> &equations, const AlignmentMatrix &pose) {
    Vec p{};
    std::size_t count = 0;
    for (const auto &e : equations)
        if (!e.isVector) {
            p += Transform(pose, e.source);
            ++count;
        }
    return count ? p / static_cast<double>(count) : Vec{};
}
void CheckWork(const AlignmentWork &work) {
    if (AlignmentGeometryFit::GetWorkStatus(work) != AlignmentStatus::FullyDetermined)
        throw std::runtime_error("Cancelled or deadline reached.");
}
bool Project(const AlignmentWork &work, const std::vector<Equation> &equations,
             const std::vector<std::size_t> &hard, const std::vector<double> &frozen,
             AlignmentMatrix &pose) {
    if (hard.empty())
        return true;
    for (std::size_t iteration = 0; iteration < work.recipe.iterationLimit; ++iteration) {
        CheckWork(work);
        const auto pivot = Pivot(equations, pose);
        const auto values = Values(equations, pose, hard, frozen);
        if (cv::norm(values, cv::NORM_INF) <= work.recipe.solveTolerance)
            return true;
        const auto rank = Decompose(Jacobian(equations, pose, hard, work.recipe.lengthScale, pivot),
                                    work.recipe.rankTolerance);
        cv::Mat dx = -rank.inverse * values;
        const double norm = cv::norm(dx);
        if (norm > work.recipe.lengthScale)
            dx *= work.recipe.lengthScale / norm;
        bool improved = false;
        for (double fraction = 1; fraction >= 1.0 / 1024; fraction *= 0.5) {
            const auto candidate =
                Multiply(Increment(dx * fraction, work.recipe.lengthScale, pivot), pose);
            if (cv::norm(Values(equations, candidate, hard, frozen)) < cv::norm(values)) {
                pose = candidate;
                improved = true;
                break;
            }
        }
        if (!improved)
            return false;
    }
    return cv::norm(Values(equations, pose, hard, frozen), cv::NORM_INF) <=
           work.recipe.solveTolerance;
}
double SoftCost(const AlignmentWork &work, const std::vector<Equation> &eq,
                const std::vector<std::size_t> &indices, const AlignmentMatrix &pose) {
    double cost = 0;
    for (const auto index : indices) {
        const auto &e = eq[index];
        const double r = std::abs(Residual(e, pose)), h = work.recipe.huberDistance;
        cost += e.weight * (r <= h ? r * r : 2 * h * r - h * h);
    }
    return cost;
}
bool Optimize(const AlignmentWork &work, const std::vector<Equation> &eq, AlignmentMatrix &pose,
              std::size_t &iterations) {
    std::vector<std::size_t> fixed;
    std::map<std::uint32_t, std::vector<std::size_t>> priorities;
    std::vector<double> frozen(eq.size(), 0);
    for (std::size_t i = 0; i < eq.size(); ++i)
        if (eq[i].isUsed) {
            if (eq[i].kind == AlignmentConstraintKind::Hard)
                fixed.push_back(i);
            else if (eq[i].kind == AlignmentConstraintKind::Soft)
                priorities[eq[i].priority].push_back(i);
        }
    if (!Project(work, eq, fixed, frozen, pose))
        return false;
    for (const auto &group : priorities) {
        bool converged = false;
        const auto &soft = group.second;
        for (std::size_t iteration = 0; iteration < work.recipe.iterationLimit; ++iteration) {
            CheckWork(work);
            ++iterations;
            const auto pivot = Pivot(eq, pose);
            const auto hardRank =
                Decompose(Jacobian(eq, pose, fixed, work.recipe.lengthScale, pivot),
                          work.recipe.rankTolerance);
            if (hardRank.nullspace.cols == 0) {
                converged = true;
                break;
            }
            cv::Mat jac =
                Jacobian(eq, pose, soft, work.recipe.lengthScale, pivot) * hardRank.nullspace;
            cv::Mat residual = Values(eq, pose, soft, std::vector<double>(eq.size(), 0));
            for (int i = 0; i < jac.rows; ++i) {
                const auto index = soft[static_cast<std::size_t>(i)];
                const double r = std::abs(residual.at<double>(i));
                const double weight =
                    std::sqrt(eq[index].weight *
                              (r > work.recipe.huberDistance ? work.recipe.huberDistance / r : 1));
                jac.row(i) *= weight;
                residual.at<double>(i) *= weight;
            }
            const auto rank = Decompose(jac, work.recipe.rankTolerance);
            cv::Mat dx = -hardRank.nullspace * rank.inverse * residual;
            if (cv::norm(dx) <= work.recipe.solveTolerance) {
                converged = true;
                break;
            }
            if (cv::norm(dx) > work.recipe.lengthScale)
                dx *= work.recipe.lengthScale / cv::norm(dx);
            const double cost = SoftCost(work, eq, soft, pose);
            bool improved = false;
            for (double fraction = 1; fraction >= 1.0 / 1024; fraction *= 0.5) {
                auto candidate =
                    Multiply(Increment(dx * fraction, work.recipe.lengthScale, pivot), pose);
                if (Project(work, eq, fixed, frozen, candidate) &&
                    SoftCost(work, eq, soft, candidate) < cost) {
                    pose = candidate;
                    improved = true;
                    break;
                }
            }
            if (!improved) {
                converged = cv::norm(jac.t() * residual) <= work.recipe.solveTolerance;
                break;
            }
        }
        if (!converged)
            return false;
        // 前一优先级取得的各项残差固定为等式，下一优先级只能沿其零空间移动。
        for (const auto i : soft) {
            frozen[i] = Residual(eq[i], pose);
            fixed.push_back(i);
        }
    }
    return true;
}
void AddDatum(std::vector<Equation> &eq, const AlignmentGeometry &g, std::size_t stage,
              const AlignmentRecipe &r) {
    const auto point = Vector(g.sourceCenter), normal = Vector(g.sourceDirection) * r.lengthScale;
    const auto add = [&](Vec p, Vec q, Vec d, bool vector, const char *id) {
        eq.push_back(
            {p, q, d, AlignmentConstraintKind::Hard, 0, 1, r.solveTolerance, id, vector, true});
    };
    if (stage == 0) {
        add(normal, {}, Vec(1, 0, 0), true, "A.normal-x");
        add(normal, {}, Vec(0, 1, 0), true, "A.normal-y");
        add(point, Vec(0, 0, r.datumOffsets[2]), Vec(0, 0, 1), false, "A.offset");
    }
    if (stage == 1) {
        add(normal, {}, Vec(1, 0, 0), true, "B.normal-x");
        add(point, Vec(0, r.datumOffsets[1], 0), Vec(0, 1, 0), false, "B.offset");
    }
    if (stage == 2)
        add(point, Vec(r.datumOffsets[0], 0, 0), Vec(1, 0, 0), false, "C.offset");
}
AlignmentMatrix DatumPose(const std::vector<AlignmentGeometry> &g, std::size_t count,
                          const AlignmentMatrix &initial, const AlignmentRecipe &recipe) {
    if (count == 0)
        return initial;
    const auto z = Vector(g[0].sourceDirection);
    const auto inverse = Inverse(initial);
    auto y = count > 1 ? Vector(g[1].sourceDirection) : Rotate(inverse, Vec(0, 1, 0));
    y = Unit(y - y.dot(z) * z);
    if (cv::norm(y) < 0.5)
        y = Tangent(z);
    const auto x = Unit(y.cross(z));
    y = z.cross(x);
    AlignmentMatrix pose{
        x[0], x[1], x[2], initial[3],
        y[0], y[1], y[2], initial[7],
        z[0], z[1], z[2], recipe.datumOffsets[2] - z.dot(Vector(g[0].sourceCenter)),
        0,    0,    0,    1};
    if (count > 1)
        pose[7] = recipe.datumOffsets[1] - y.dot(Vector(g[1].sourceCenter));
    if (count > 2)
        pose[3] = recipe.datumOffsets[0] - x.dot(Vector(g[2].sourceCenter));
    return pose;
}
void Diagnose(const AlignmentWork &work, const std::vector<Equation> &eq,
              AlignmentCandidate &output, bool converged) {
    auto &d = output.diagnostics;
    const auto &pose = output.sourceToTarget;
    d.lengthScale = work.recipe.lengthScale;
    d.rankTolerance = work.recipe.rankTolerance;
    std::vector<std::size_t> hard, soft;
    for (std::size_t i = 0; i < eq.size(); ++i)
        if (eq[i].isUsed) {
            if (eq[i].kind == AlignmentConstraintKind::Hard)
                hard.push_back(i);
            else if (eq[i].kind == AlignmentConstraintKind::Soft)
                soft.push_back(i);
        }
    const auto pivot = Pivot(eq, pose);
    const auto c = Decompose(Jacobian(eq, pose, hard, work.recipe.lengthScale, pivot),
                             work.recipe.rankTolerance);
    d.hardRemaining = static_cast<std::size_t>(6 - c.rank);
    d.remaining = d.hardRemaining;
    d.singularValues = c.values;
    d.condition = c.condition;
    cv::Mat nullspace = c.nullspace;
    if (!soft.empty() && nullspace.cols) {
        const auto rank =
            Decompose(Jacobian(eq, pose, soft, work.recipe.lengthScale, pivot) * nullspace,
                      work.recipe.rankTolerance);
        d.remaining -= static_cast<std::size_t>(rank.rank);
        d.singularValues.insert(d.singularValues.end(), rank.values.begin(), rank.values.end());
        d.condition = std::max(d.condition, rank.condition);
        nullspace =
            rank.nullspace.cols == 0 ? cv::Mat(6, 0, CV_64F) : cv::Mat(nullspace * rank.nullspace);
    }
    for (int j = 0; j < nullspace.cols; ++j) {
        std::array<double, 6> v{};
        for (int i = 0; i < 6; ++i)
            v[static_cast<std::size_t>(i)] = nullspace.at<double>(i, j);
        // 从以质心为旋转中心的 twist 转换到目标原点，保持局部运动意义。
        const auto correction = Vec(v[0], v[1], v[2]).cross(pivot) / work.recipe.lengthScale;
        for (int i = 0; i < 3; ++i)
            v[static_cast<std::size_t>(i + 3)] -= correction[i];
        d.nullspace.push_back(v);
    }
    double sum = 0;
    std::size_t count = 0;
    bool hardPassed = true, quality = true;
    for (const auto &e : eq) {
        const double value = Residual(e, pose);
        output.residuals.push_back({e.id, e.kind, e.priority, value, e.tolerance, e.isUsed});
        // 被排除的点仍进入最终评价摘要，不能靠剔除伪造低残差。
        if (e.kind != AlignmentConstraintKind::Hard) {
            sum += value * value;
            ++count;
        }
        d.maximum = std::max(d.maximum, std::abs(value));
        if (e.kind == AlignmentConstraintKind::Hard && std::abs(value) > e.tolerance)
            hardPassed = false;
        if (std::abs(value) > e.tolerance)
            quality = false;
    }
    d.rms = count ? std::sqrt(sum / static_cast<double>(count)) : 0;
    d.isQualityPassed = quality && d.rms <= work.recipe.maxAlignmentRms;
    if (!Rigid(pose) || !std::isfinite(d.rms))
        d.status = AlignmentStatus::InvalidInput;
    else if (!hardPassed)
        d.status = AlignmentStatus::Conflicting;
    else if (!converged)
        d.status = AlignmentStatus::NotConverged;
    else if (d.condition > work.recipe.conditionLimit)
        d.status = AlignmentStatus::Degenerate;
    else
        d.status =
            d.remaining ? AlignmentStatus::Underconstrained : AlignmentStatus::FullyDetermined;
}
AlignmentCandidate SolvePose(const AlignmentWork &work, const AlignmentMatrix &initial) {
    AlignmentCandidate output;
    output.sourceToTarget = initial;
    const auto &recipe = work.recipe;
    const bool sequential = recipe.method == AlignmentMethod::SequentialPlanes ||
                            recipe.method == AlignmentMethod::ConstrainedBestFit;
    std::vector<Equation> equations;
    std::size_t total = 0;
    for (std::size_t i = 0; i < recipe.geometries.size(); ++i) {
        CheckWork(work);
        const auto &spec = recipe.geometries[i];
        const auto samples = AlignmentGeometryFit::BuildSamples(work, spec.region, initial);
        total += samples.points.size();
        if (total > work.config.pointLimit)
            throw std::length_error("Total selection exceeds point budget.");
        std::optional<AlignmentPoint> constraint;
        bool fixed = false;
        if (sequential && i < recipe.datumCount) {
            Require(spec.kind == AlignmentGeometryKind::Plane, "Sequential datums require planes.");
            Require(spec.association == AlignmentAssociation::SequentialLeastSquares,
                    "Sequential association must be explicit.");
            if (i == 1)
                constraint = output.geometries[0].sourceDirection;
            if (i == 2) {
                constraint = Point(Vector(output.geometries[1].sourceDirection)
                                       .cross(Vector(output.geometries[0].sourceDirection)));
                fixed = true;
            }
        }
        output.geometries.push_back(
            AlignmentGeometryFit::BuildGeometry(work, spec, samples, initial, constraint, fixed));
    }
    if (sequential) {
        output.sourceToTarget = DatumPose(output.geometries, recipe.datumCount, initial, recipe);
        for (std::size_t i = 0; i < recipe.datumCount; ++i) {
            AddDatum(equations, output.geometries[i], i, recipe);
            std::vector<std::size_t> ids(equations.size());
            std::iota(ids.begin(), ids.end(), 0);
            output.diagnostics.stageRemaining.push_back(static_cast<std::size_t>(
                6 - Decompose(Jacobian(equations, output.sourceToTarget, ids, recipe.lengthScale,
                                       Pivot(equations, output.sourceToTarget)),
                              recipe.rankTolerance)
                        .rank));
        }
    }
    if (recipe.method == AlignmentMethod::PlaneTwoHoles) {
        const auto &g = output.geometries;
        Require(g[0].kind == AlignmentGeometryKind::Plane, "PlaneTwoHoles requires an A plane.");
        Require((g[1].kind == AlignmentGeometryKind::Cylinder ||
                 g[1].kind == AlignmentGeometryKind::Line) &&
                    (g[2].kind == AlignmentGeometryKind::Cylinder ||
                     g[2].kind == AlignmentGeometryKind::Line),
                "PlaneTwoHoles requires two axes.");
        const auto origin = GeometryPoint(g[1], g[0]), directionPoint = GeometryPoint(g[2], g[0]);
        const auto z = Vector(g[0].sourceDirection);
        const auto x = Unit(directionPoint - origin);
        Require(cv::norm(directionPoint - origin) > recipe.solveTolerance &&
                    cv::norm(x.cross(z)) > 0.9,
                "Degenerate hole direction.");
        const auto y = z.cross(x);
        output.sourceToTarget = {x[0], x[1], x[2], recipe.datumOffsets[0] - x.dot(origin),
                                 y[0], y[1], y[2], recipe.datumOffsets[1] - y.dot(origin),
                                 z[0], z[1], z[2], recipe.datumOffsets[2] - z.dot(origin),
                                 0,    0,    0,    1};
        AddDatum(equations, g[0], 0, recipe);
        equations.push_back({origin, Vector(recipe.datumOffsets), Vec(1, 0, 0),
                             AlignmentConstraintKind::Hard, 0, 1, recipe.solveTolerance,
                             "B.center-x"});
        equations.push_back({origin, Vector(recipe.datumOffsets), Vec(0, 1, 0),
                             AlignmentConstraintKind::Hard, 0, 1, recipe.solveTolerance,
                             "B.center-y"});
        equations.push_back({Unit(directionPoint - origin) * recipe.lengthScale,
                             {},
                             Vec(0, 1, 0),
                             AlignmentConstraintKind::Hard,
                             0,
                             1,
                             recipe.solveTolerance,
                             "C.direction",
                             true});
        output.diagnostics.stageRemaining = {3, 1, 0};
    }
    for (const auto &constraint : recipe.constraints) {
        const auto &g = output.geometries[constraint.geometryIndex];
        Require(g.kind != AlignmentGeometryKind::Plane,
                "Plane centroid is not an RPS construction point.");
        const auto p = GeometryPoint(
            g, constraint.sectionPlane
                   ? std::optional<AlignmentGeometry>(output.geometries[*constraint.sectionPlane])
                   : std::nullopt);
        equations.push_back({p, Vector(constraint.nominalPoint), Vector(constraint.targetDirection),
                             constraint.kind, constraint.priority, constraint.weight,
                             constraint.tolerance, g.id});
    }
    if (recipe.method == AlignmentMethod::ConstrainedBestFit) {
        Require(recipe.exactMesh == work.input.mesh,
                "Best-fit correspondence belongs to another mesh revision.");
        const auto &v = work.mesh->GetVertices();
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < recipe.fitPairs.size(); ++i) {
            CheckWork(work);
            const auto &pair = recipe.fitPairs[i];
            Require(pair.vertexId < v.size() / 3, "Best-fit index out of bounds.");
            const auto index = static_cast<std::size_t>(pair.vertexId);
            const Vec p(v[index * 3], v[index * 3 + 1], v[index * 3 + 2]);
            const auto q = Vector(pair.nominalPoint);
            bool used = AlignmentGeometryFit::GetPointUsable(work, index) &&
                        cv::norm(Transform(output.sourceToTarget, p) - q) <= recipe.maxPairDistance;
            if (recipe.minPairNormalCosine) {
                const MeshAttribute *normal = nullptr;
                for (const auto &attribute : work.mesh->GetPointAttributes())
                    if (attribute.name == "measurement.normal")
                        normal = &attribute;
                Require(pair.targetNormal && normal && normal->componentCount == 3 &&
                            normal->values.size() == v.size(),
                        "Normal correspondence gate requires source and target normals.");
                const Vec measured(normal->values[3 * index], normal->values[3 * index + 1],
                                   normal->values[3 * index + 2]);
                used = used && Finite(measured) && std::abs(cv::norm(measured) - 1) <= 1e-6 &&
                       Rotate(output.sourceToTarget, measured).dot(Vector(*pair.targetNormal)) >=
                           *recipe.minPairNormalCosine;
            }
            if (used)
                ++accepted;
            else
                ++output.diagnostics.rejectedPairs;
            for (int axis = 0; axis < (pair.targetNormal ? 1 : 3); ++axis) {
                Vec direction{};
                direction[axis] = 1;
                if (pair.targetNormal)
                    direction = Vector(*pair.targetNormal);
                equations.push_back({p, q, direction, AlignmentConstraintKind::Soft, 63,
                                     pair.weight, recipe.maxAlignmentRms,
                                     "pair-" + std::to_string(i) + "-" + std::to_string(axis),
                                     false, used});
            }
        }
        Require(static_cast<double>(accepted) / static_cast<double>(recipe.fitPairs.size()) >=
                    recipe.minPairCoverage,
                "Best-fit coverage failed.");
    }
    const bool converged =
        Optimize(work, equations, output.sourceToTarget, output.diagnostics.iterations);
    Diagnose(work, equations, output, converged);
    // 对应集合在初始化后固定以保证可重复性；最终姿态重新验证距离门槛。
    for (const auto &e : equations) {
        if (e.kind == AlignmentConstraintKind::Soft && e.priority == 63 && e.isUsed &&
            cv::norm(Transform(output.sourceToTarget, e.source) - e.target) >
                recipe.maxPairDistance) {
            output.diagnostics.isQualityPassed = false;
            output.diagnostics.message = "Final correspondence distance exceeds the recipe gate.";
        }
    }
    return output;
}
} // namespace

AlignmentCandidate AlignmentSolver::BuildResult(const AlignmentWork &work) {
    AlignmentCandidate result;
    try {
        Require(AlignmentGeometryFit::GetRecipeValid(work.recipe, work.config), "Invalid recipe.");
        Require(work.mesh && work.mesh->GetVertices().size() % 3 == 0 &&
                    work.mesh->GetCoordinateFrame() == work.input.coordinateFrame && work.input.unit == work.recipe.unit,
                "Invalid mesh or incompatible spatial units.");
        Require(!work.poses.empty() && work.poses.size() <= 8,
                "Expected one to eight initial poses.");
        // 包含薄 SVD/Jacobian、点集副本、选择索引、配方及候选；配置上限独立限界。
        if (work.config.workingBytes < 65536 ||
            work.config.pointLimit + work.config.constraintLimit >
                (work.config.workingBytes - 65536) / 1024) {
            result.diagnostics.status = AlignmentStatus::BudgetExceeded;
            return result;
        }
        for (const auto &g : work.recipe.geometries)
            if (g.association == AlignmentAssociation::Contact) {
                result.diagnostics.status = AlignmentStatus::Unsupported;
                result.diagnostics.message = "Contact association is unsupported.";
                return result;
            }
        bool hasCandidate = false;
        std::vector<AlignmentMatrix> validPoses;
        for (const auto &pose : work.poses) {
            CheckWork(work);
            Require(Rigid(pose), "Initial pose must be rigid.");
            AlignmentCandidate candidate;
            try {
                candidate = SolvePose(work, pose);
            } catch (const std::invalid_argument &error) {
                candidate.diagnostics.status = AlignmentStatus::Degenerate;
                candidate.diagnostics.message = error.what();
            }
            if (candidate.diagnostics.status == AlignmentStatus::FullyDetermined &&
                candidate.diagnostics.isQualityPassed) {
                bool distinct = true;
                for (const auto &existing : validPoses) {
                    const auto relative = Multiply(candidate.sourceToTarget, Inverse(existing));
                    const double cosine =
                        std::clamp((relative[0] + relative[5] + relative[10] - 1) / 2, -1.0, 1.0);
                    const double distance = cv::norm(Vec(relative[3], relative[7], relative[11])) +
                                            work.recipe.lengthScale * std::acos(cosine);
                    if (distance < work.recipe.solveTolerance * 10)
                        distinct = false;
                }
                if (distinct)
                    validPoses.push_back(candidate.sourceToTarget);
            }
            const auto score = [](const AlignmentDiagnostics &value) {
                if (value.status == AlignmentStatus::FullyDetermined)
                    return value.isQualityPassed ? 3 : 2;
                return value.status == AlignmentStatus::Underconstrained ? 1 : 0;
            };
            if (!hasCandidate || score(candidate.diagnostics) > score(result.diagnostics) ||
                (score(candidate.diagnostics) == score(result.diagnostics) &&
                 candidate.diagnostics.rms < result.diagnostics.rms)) {
                result = std::move(candidate);
                hasCandidate = true;
            }
        }
        if (validPoses.size() > 1) {
            result.diagnostics.status = AlignmentStatus::Ambiguous;
            result.diagnostics.isQualityPassed = false;
            result.diagnostics.message = "Multiple distinct candidate poses satisfy the recipe.";
        }
    } catch (const std::length_error& error) {
        result.diagnostics.status = AlignmentStatus::BudgetExceeded;
        result.diagnostics.message = error.what();
    } catch (const std::bad_alloc &) {
        result.diagnostics.status = AlignmentStatus::BudgetExceeded;
    } catch (const std::invalid_argument &error) {
        result.diagnostics.status = AlignmentStatus::InvalidInput;
        result.diagnostics.message = error.what();
    } catch (const std::exception &error) {
        const auto status = AlignmentGeometryFit::GetWorkStatus(work);
        result.diagnostics.status =
            status != AlignmentStatus::FullyDetermined ? status : AlignmentStatus::InternalError;
        result.diagnostics.message = error.what();
    }
    return result;
}
