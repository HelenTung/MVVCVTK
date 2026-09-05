#include "AlignmentGeometry.h"
#include "AlignmentMath.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

int main(int argc, char **argv) {
    using namespace AlignmentMath;
    if (argc != 2) {
        std::cerr << "Usage: AlignmentReferenceTests <NIST reference-pairs directory>\n";
        return 2;
    }
    const std::map<std::string, AlignmentGeometryKind> kinds{
        {"Line2d", AlignmentGeometryKind::Line},   {"Line3d", AlignmentGeometryKind::Line},
        {"Plane", AlignmentGeometryKind::Plane},   {"Circle2d", AlignmentGeometryKind::Circle},
        {"Sphere", AlignmentGeometryKind::Sphere}, {"Cylinder", AlignmentGeometryKind::Cylinder}};
    std::size_t count = 0, failures = 0;
    for (const auto &folder : kinds) {
        std::size_t folderCount = 0;
        const auto path = std::filesystem::path(argv[1]) / folder.first;
        if (!std::filesystem::is_directory(path)) {
            std::cerr << "Missing dataset folder: " << path << '\n';
            return 2;
        }
        for (const auto &entry : std::filesystem::directory_iterator(path)) {
            if (entry.path().extension() != ".ds")
                continue;
            ++count;
            ++folderCount;
            std::ifstream data(entry.path());
            std::size_t n = 0;
            data >> n;
            if (!data || n == 0 || n > 500) {
                std::cerr << "Invalid NIST sample count\n";
                return 2;
            }
            AlignmentSamples samples;
            for (std::size_t i = 0; i < n; ++i) {
                AlignmentPoint p;
                data >> p[0] >> p[1] >> p[2];
                if (!data)
                    return 2;
                samples.points.push_back(p);
            }
            auto fitPath = entry.path();
            fitPath.replace_extension(".fit");
            std::ifstream fit(fitPath);
            std::vector<double> reference;
            double number = 0;
            while (fit >> number)
                reference.push_back(number);
            const bool sphere = folder.second == AlignmentGeometryKind::Sphere;
            const bool round = folder.second == AlignmentGeometryKind::Circle ||
                               folder.second == AlignmentGeometryKind::Cylinder || sphere;
            if (reference.size() != (sphere ? 4U : (round ? 7U : 6U))) {
                std::cerr << "Invalid NIST fit parameter count: " << fitPath << '\n';
                return 2;
            }
            const Vec refCenter(reference[0], reference[1], reference[2]);
            const Vec refNormal =
                sphere ? Vec(0, 0, 1) : Vec(reference[3], reference[4], reference[5]);
            AlignmentWork work;
            work.recipe.iterationLimit = 200;
            work.recipe.solveTolerance = 1e-8;
            work.recipe.rankTolerance = 1e-12;
            work.recipe.conditionLimit = 1e12;
            Vec centroid{};
            for (const auto &p : samples.points)
                centroid += Vector(p);
            centroid /= static_cast<double>(n);
            double spread = 0;
            for (const auto &p : samples.points)
                spread += (Vector(p) - centroid).dot(Vector(p) - centroid);
            work.recipe.lengthScale = std::max(1.0, std::sqrt(spread / static_cast<double>(n)));
            work.cancelled = std::make_shared<std::atomic<bool>>(false);
            work.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            AlignmentGeometrySpec spec;
            spec.id = entry.path().stem().string();
            spec.kind = folder.second;
            spec.maxFitRms = 1000;
            spec.targetDirection = Point(refNormal);
            // 圆柱的方向是明确的初始提示；加入偏转，不能把参考解直接作为输出。
            if (folder.second == AlignmentGeometryKind::Cylinder)
                spec.targetDirection = Point(Unit(refNormal + Tangent(refNormal) * 0.01));
            try {
                const auto result =
                    AlignmentGeometryFit::BuildGeometry(work, spec, samples, alignmentIdentity);
                const auto center = Vector(result.sourceCenter),
                           normal = Vector(result.sourceDirection);
                const double angle = sphere ? 0 : cv::norm(normal.cross(refNormal));
                double position = 0;
                if (folder.second == AlignmentGeometryKind::Plane)
                    position = std::abs((center - refCenter).dot(refNormal));
                else if (folder.second == AlignmentGeometryKind::Line ||
                         folder.second == AlignmentGeometryKind::Cylinder)
                    position = cv::norm((center - refCenter).cross(refNormal));
                else
                    position = cv::norm(center - refCenter);
                const double radiusError =
                    round ? std::abs(result.radius - reference.back() / 2) : 0;
                // 输入坐标截断至 1e-5 mm；比较几何实体，避免比较无限轴任意锚点。
                if (position > 1e-4 || radiusError > 1e-4 || angle > 1e-5) {
                    ++failures;
                    std::cerr << entry.path().filename() << " position=" << position
                              << " radius=" << radiusError << " angle=" << angle << '\n';
                }
            } catch (const std::exception &error) {
                ++failures;
                std::cerr << entry.path().filename() << ": " << error.what() << '\n';
            }
        }
        std::cout << folder.first << ": " << folderCount << " reference pairs\n";
    }
    std::cout << count << " cases, " << failures
              << " failures; unofficial self-test, not NIST certification.\n";
    std::cout << "Circle3d excluded: NIST uses projected-plane association; this feature uses full "
                 "3D distances. Cone unsupported.\n";
    return failures || count != 180 ? 1 : 0;
}
