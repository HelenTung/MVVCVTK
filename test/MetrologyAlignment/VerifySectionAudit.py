"""真实 C3 截面的独立数值复核；numpy/scipy 仅为可选仓内验证依赖，不是 SDK 依赖。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
import scipy
from scipy.optimize import least_squares


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def contour(points, triangles, origin, normal):
    """逐边线性求交，再由原始顶点编号建立独立邻接图。参考数据不得有落在平面上的边。"""
    locations, adjacency = {}, {}
    for triangle in triangles:
        positions = points[triangle]
        distances = (positions - origin) @ normal
        if np.all(distances > 0) or np.all(distances < 0):
            continue
        require(np.all(np.abs(distances) > 1e-12), "Reference plane crosses a vertex; this reference method is inapplicable")
        keys = []
        for j in range(3):
            k = (j + 1) % 3
            if distances[j] * distances[k] < 0:
                key = tuple(sorted((int(triangle[j]), int(triangle[k]))))
                locations[key] = positions[j] + distances[j] / (distances[j] - distances[k]) * (positions[k] - positions[j])
                keys.append(key)
        require(len(keys) == 2, "Reference triangle intersection is not a segment")
        for a, b in (keys, keys[::-1]):
            adjacency.setdefault(a, []).append(b)
    require(adjacency and all(len(v) == 2 for v in adjacency.values()), "Reference contour is open or branched")
    first = min(locations, key=lambda k: tuple(locations[k]))
    previous, current = first, min(adjacency[first], key=lambda k: tuple(locations[k]))
    order = [first]
    while current != first:
        require(current not in order, "Reference contour does not form a simple cycle")
        order.append(current)
        neighbors = adjacency[current]
        previous, current = current, neighbors[0] if neighbors[0] != previous else neighbors[1]
    require(len(order) == len(locations), "Reference contains multiple contours")
    return np.array([locations[k] for k in order])


def verify(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    require(data["schema"] == "ct-timepoint-audit-1" and data["pipelineChecksPassed"], "Public pipeline failed or schema is unknown")
    scans = {scan["id"]: scan for scan in data["scans"]}
    require(len(scans) == len(data["scans"]) == len(data["sectionInspections"]), "Scan coverage mismatch")
    require(set(scans) == {audit["scanId"] for audit in data["sectionInspections"]}, "Section scan identity mismatch")
    folder = path.parent
    result = {"scope": "independent numerical verification on measured meshes, not calibrated physical accuracy",
              "numpy": np.__version__, "scipy": scipy.__version__, "python": sys.version,
              "observationsSha256": digest(path), "verifierSha256": digest(Path(__file__)),
              "sections": [], "qualityRejectionsVerified": [], "notApplicable": [], "inputs": {}}
    for audit in data["sectionInspections"]:
        eligible = [o for o in scans[audit["scanId"]]["objectGeometry"] if o["closed"] and o["manifold"] and not o["truncated"]]
        if not audit["attempted"]:
            require(not eligible, "Eligible object skipped without attempting sections")
            result["notApplicable"].append({"scan": audit["scanId"], "reason": audit["reason"]})
            continue
        require(eligible and audit["status"] == 0 and audit["objectIndex"] == max(eligible, key=lambda o: o["triangleCount"])["index"],
                "Selected object or inspection status does not match the declared rule")
        require(len(audit["sections"]) == 3 and {s["axis"] for s in audit["sections"]} == {0, 1, 2}, "Missing or duplicate midpoint planes")
        require(audit["successfulSections"] == sum(s["status"] == 0 for s in audit["sections"]) > 0,
                "Reported successful section count is inconsistent")
        vertex_path, triangle_path = folder / audit["sourcePoints"], folder / audit["sourceTriangles"]
        vertex_rows = np.loadtxt(vertex_path, delimiter=",", skiprows=1, ndmin=2)
        triangle_rows = np.loadtxt(triangle_path, delimiter=",", skiprows=1, ndmin=2)
        require(vertex_rows.shape[1] == 4 and len(vertex_rows) > 0 and np.all(np.isfinite(vertex_rows)), "Invalid source point CSV")
        require(triangle_rows.shape[1] == 5 and len(triangle_rows) > 0 and np.all(np.isfinite(triangle_rows)) and
                np.all(triangle_rows == np.floor(triangle_rows)), "Invalid source triangle CSV")
        require(np.all(vertex_rows[:, 0] == np.floor(vertex_rows[:, 0])), "Non-integral point ids")
        triangle_rows = triangle_rows.astype(np.int64)
        point_ids = vertex_rows[:, 0].astype(np.int64)
        require(np.array_equal(point_ids, np.arange(point_ids[0], point_ids[0] + len(point_ids))), "Source vertex ids are not contiguous")
        triangle_ids = triangle_rows[:, 0]
        require(np.array_equal(triangle_ids, np.arange(triangle_ids[0], triangle_ids[0] + len(triangle_ids))), "Source triangle ids are not contiguous")
        points = vertex_rows[:, 1:]
        triangles = triangle_rows[:, 1:4] - point_ids[0]
        validity = triangle_rows[:, 4]
        require(np.all((validity == 0) | (validity == 1)), "Invalid face quality values")
        require(np.min(triangles) >= 0 and np.max(triangles) < len(points), "Source triangle index is out of range")
        for file in (vertex_path, triangle_path):
            result["inputs"][file.name] = digest(file)
        transform = np.array(audit["sourceToTarget"]).reshape(4, 4)
        require(np.all(np.isfinite(transform)) and np.max(np.abs(transform[3] - [0, 0, 0, 1])) < 1e-10 and
                np.linalg.norm(transform[:3, :3].T @ transform[:3, :3] - np.eye(3)) < 1e-8 and
                abs(np.linalg.det(transform[:3, :3]) - 1) < 1e-8, "Source transform is not rigid")
        inverse = np.linalg.inv(transform)
        for section in audit["sections"]:
            origin = (inverse @ np.r_[section["planePoint"], 1])[:3]
            normal = inverse[:3, section["axis"]]
            normal /= np.linalg.norm(normal)
            require(np.isfinite(section["sampleSpacing"]) and section["sampleSpacing"] > 0 and
                    np.isfinite(section["planeTolerance"]) and section["planeTolerance"] > 0, "Invalid sampling controls")
            plane_distances = (points[triangles] - origin) @ normal
            crossing = (plane_distances.min(axis=1) < -section["planeTolerance"]) & (plane_distances.max(axis=1) > section["planeTolerance"])
            invalid_crossings = int(np.sum(crossing & (validity == 0)))
            if section["status"] != 0:
                require(section["status"] == 2 and section["message"] == "Selected section crosses invalid measurement faces." and invalid_crossings > 0,
                        "Rejected section lacks independent crossed-face quality evidence")
                result["qualityRejectionsVerified"].append({"scan": audit["scanId"], "axis": section["axis"], "invalidCrossedFaces": invalid_crossings})
                continue
            require(invalid_crossings == 0, "Successful section crosses invalid quality faces")
            loop = contour(points, triangles, origin, normal)
            edges = np.roll(loop, -1, axis=0) - loop
            lengths = np.linalg.norm(edges, axis=1)
            require(np.all(np.isfinite(lengths)) and np.all(lengths > 0), "Invalid contour edge lengths")
            perimeter = float(np.sum(lengths))
            # 数值比较门限来自请求的平面容差及默认求解容差 1e-8 mm，不是 CT 测量精度。
            tolerance = max(4 * section["planeTolerance"], 10 * 1e-8,
                            128 * np.finfo(float).eps * np.max(np.abs(points)) * len(loop))
            require(abs(perimeter - section["perimeter"]) <= tolerance, "Independent perimeter mismatch")
            count = max(8, int(np.ceil(perimeter / section["sampleSpacing"])))
            require(count == section["sampleCount"], "Independent sample count mismatch")
            boundaries = np.r_[0, np.cumsum(lengths)]
            distances = np.arange(count) * perimeter / count
            indices = np.minimum(np.searchsorted(boundaries, distances, side="right") - 1, len(loop) - 1)
            independent = loop[indices] + ((distances - boundaries[indices]) / lengths[indices])[:, None] * edges[indices]
            sample_path = folder / section["samples"]
            samples = np.loadtxt(sample_path, delimiter=",", skiprows=1, ndmin=2)
            require(samples.shape == (count, 5) and np.all(np.isfinite(samples)) and np.all(samples[:, 0] == np.floor(samples[:, 0])),
                    "Invalid provenance CSV schema or integer ids")
            ids = samples[:, 0].astype(np.int64) - triangle_ids[0]
            require(np.min(ids) >= 0 and np.max(ids) < len(triangles), "Published provenance is out of range")
            require(np.min(samples[:, 1:4]) >= -1e-10 and np.max(samples[:, 1:4]) <= 1 + 1e-10 and
                    np.max(np.abs(samples[:, 4])) <= 2 * section["planeTolerance"] and
                    np.max(np.abs(samples[:, 1:4].sum(axis=1) - 1)) < 1e-10,
                    "Published barycentric schema mismatch")
            reconstructed = np.einsum("ij,ijk->ik", samples[:, 1:4], points[triangles[ids]]) - samples[:, 4, None] * normal
            sample_error = float(np.max(np.linalg.norm(independent - reconstructed, axis=1)))
            require(sample_error <= tolerance, "Independent arclength sample locations mismatch")
            # 在独立求交/等弧长样本上调用 SciPy LM，产品拟合器使用 OpenCV 自有求解路径。
            seed_axis = np.eye(3)[int(np.argmin(np.abs(normal)))]
            u = np.cross(normal, seed_axis); u /= np.linalg.norm(u)
            basis = np.column_stack((u, np.cross(normal, u)))
            mean = np.mean(independent, axis=0)
            xy = (independent - mean) @ basis
            initial = np.linalg.lstsq(np.column_stack((2 * xy, np.ones(count))), np.sum(xy * xy, axis=1), rcond=None)[0]
            radius = np.sqrt(initial[2] + initial[:2] @ initial[:2])
            solve = least_squares(lambda x: np.linalg.norm(xy - x[:2], axis=1) - x[2],
                                  np.r_[initial[:2], radius], method="lm", ftol=1e-12, xtol=1e-12, gtol=1e-12, max_nfev=2000)
            require(solve.success and solve.x[2] > 0, "Independent circle solver did not converge")
            center = mean + basis @ solve.x[:2]
            center_error = float(np.linalg.norm(center - section["sourceCenter"]))
            radius_error = float(abs(solve.x[2] - section["radius"]))
            rms = float(np.sqrt(np.mean(solve.fun ** 2)))
            require(center_error <= tolerance and radius_error <= tolerance and abs(rms - section["fitRms"]) <= tolerance,
                    "Independent circle center/radius/RMS mismatch")
            support = section["support"]
            spreads = np.linalg.svd(independent - mean, compute_uv=False) / np.sqrt(count)
            require(np.max(np.abs(spreads - support["principalRmsSpreads"])) <= tolerance,
                    "Independent principal sample spreads mismatch")
            angles = np.sort(np.arctan2((independent - center) @ basis[:, 1], (independent - center) @ basis[:, 0]))
            angular_span = float(360 - np.max(np.diff(np.r_[angles, angles[0] + 2 * np.pi])) * 180 / np.pi)
            angular_tolerance = max(1e-8, 2 * tolerance / solve.x[2] * 180 / np.pi)
            require(abs(angular_span - support["angularSpanDegrees"]) <= angular_tolerance and
                    "axialSpan" not in support and "radialDirectionMeanLength" not in support,
                    "Independent angular sample support mismatch")
            result["inputs"][sample_path.name] = digest(sample_path)
            result["sections"].append({"input": audit["sourcePoints"], "axis": section["axis"], "sampleCount": count,
                "perimeterMm": perimeter, "radiusMm": float(solve.x[2]), "fitRmsMm": rms,
                "sampleMaximumErrorMm": sample_error, "centerErrorMm": center_error, "radiusErrorMm": radius_error,
                "numericalToleranceMm": tolerance, "principalRmsSpreadsMm": spreads.tolist(), "angularSpanDegrees": angular_span})
    require(result["sections"], "No real sections independently verified")
    result["numericalChecksPassed"] = True
    result["physicalAccuracyValidated"] = False
    destination = folder / "section-reference.json"
    require(not destination.exists(), "Reference evidence already exists; use a new audit run")
    destination.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"REFERENCE {destination}: {len(result['sections'])} real sections verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("observations", nargs="?")
    parser.add_argument("--audit")
    parser.add_argument("--manifest")
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.audit:
        require(args.manifest and args.output and not args.observations, "Audit needs manifest and output")
        command = [args.audit, args.manifest, args.output, "--controls-only", "--new-run"]
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", check=False)
        print(run.stdout, end="")
        require(run.returncode == 0, f"Audit exited {run.returncode}")
        reports = [line[len("OBSERVATIONS "):] for line in run.stdout.splitlines() if line.startswith("OBSERVATIONS ")]
        require(len(reports) == 1, "Audit did not identify one observation report")
        verify(Path(reports[0]))
    else:
        require(args.observations is not None, "Observation file required")
        verify(Path(args.observations))
