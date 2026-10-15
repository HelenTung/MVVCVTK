"""使用标准库解析器核对 JSON 全报告与 CSV 统计视图的一致性。"""
import argparse
import csv
import colorsys
import hashlib
import json
import math
import sys
from pathlib import Path


def require(value, message):
    if not value:
        raise ValueError(message)


def text(value):
    # CSV 规定对公式前缀和原始单引号统一增加一个单引号，可无歧义还原。
    return value[1:] if value.startswith("'") else value


def verify_report(json_path, csv_path, fixture=False):
    def constant(value):
        raise ValueError("Nonstandard JSON numeric token: " + value)
    report = json.loads(json_path.read_text(encoding="utf-8"), parse_constant=constant)
    require(report["format"] == "mvvcvtk.metrology.report.json/3", "Unknown report format")
    alignments = {a["result"]: a for a in report["alignments"]}
    inspections = {v["result"]: v for v in report["inspections"]}
    deviations = {v["result"]: v for v in report["deviations"]}
    volumes = {v["result"]: v for v in report["voxelVolumes"]}
    require(len(volumes) == len(report["voxelVolumes"]), "Duplicate volume identity")
    with csv_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        require(len(reader.fieldnames) == 47 and len(set(reader.fieldnames)) == 47, "CSV column schema mismatch")
        rows = list(reader)
    for row in rows:
        require(row["configuration_json"] != "" if row["record_type"] in ("deviation-display","scene-display") else row["configuration_json"] == "", "Unexpected/missing configuration JSON cell")
        require(None not in row and all(v is not None for v in row.values()), "CSV record has wrong column count")
        require(text(row["report_id"]) == report["id"] and int(row["captured_commit"]) == report["capturedCommit"], "CSV report identity mismatch")
        require(row["all_inputs_current_at_capture"] == str(report["wereInputsCurrentAtCapture"]).lower(), "CSV capture currentness mismatch")
        if row["record_type"] == "voxel-volume":
            require(all(row[k] == "" for k in ("alignment", "transform", "source_mesh", "reference", "source_frame", "target_frame",
                    "source_to_target", "alignment_quality_passed", "nominal", "deviation", "lower_limit", "upper_limit", "verdict")),
                    "Voxel CSV row fabricates alignment, nominal or tolerance")
            continue
        require(all(row[k] == "" for k in ("source_volume", "validity_image", "voxel_method", "voxel_extent", "voxel_range", "voxel_labels",
                    "voxel_spacing", "voxel_origin", "voxel_direction", "require_enclosed_selection", "touches_region_boundary", "algorithm_version")),
                "Nonvoxel CSV row contains voxel parameters")
        owner = alignments[text(row["alignment"])]
        require(json.loads(row["source_to_target"]) == owner["sourceToTarget"] and text(row["transform"]) == owner["transform"] and
                text(row["source_mesh"]) == owner["input"]["mesh"] and text(row["reference"]) == owner["recipe"]["nominalData"],
                "CSV coordinate owner mismatch")
        require(row["all_inputs_current_at_capture"] == str(report["wereInputsCurrentAtCapture"]).lower(), "CSV capture currentness mismatch")
        require(row["alignment_quality_passed"] == str(owner["diagnostics"]["isQualityPassed"]).lower(), "CSV alignment quality mismatch")
    def number(cell, expected):
        require(cell == "" if expected is None else cell != "" and float(cell) == expected, "CSV optional numeric value mismatch")
    dimension_rows = [row for row in rows if row["record_type"] == "inspection"]
    require(len(dimension_rows) == sum(len(v["report"]["items"]) for v in inspections.values()), "CSV dimension count mismatch")
    for row in dimension_rows:
        snapshot = inspections[text(row["result"])]
        found = [i for i, value in enumerate(snapshot["report"]["items"]) if value["id"] == text(row["item_id"])]
        require(len(found) == 1, "CSV item identity ambiguous")
        i = found[0]; value = snapshot["report"]["items"][i]; spec = snapshot["plan"]["items"][i]
        require(row["verdict"] == value["verdict"] and text(row["name"]) == spec["name"] and row["metric"] == spec["measured"]["metric"], "CSV item metadata mismatch")
        for key in ("measured", "nominal", "deviation"):
            number(row[key], value[key])
        limits = spec["deviationLimits"]
        number(row["lower_limit"], limits[0] if limits else None); number(row["upper_limit"], limits[1] if limits else None)
        require(text(row["message"]) == value["message"], "CSV item message mismatch")
    samples_verified = 0
    for value in deviations.values():
        require(value["parameters"]["computeBackend"] in ("Cpu","Cuda"), "Unknown or missing deviation compute backend")
        for direction in ("forward", "reverse"):
            field = value[direction]
            if field is None:
                continue
            if field["samplesIncluded"]:
                require(len(field["samples"]) == field["sampleCount"], "JSON sample coverage mismatch")
                for sample in field["samples"]:
                    if sample["state"] in ("NotSelected", "InvalidSource"):
                        require(sample["value"] is None and sample["distance"] is None and sample["closestTriangle"] is None, "Invalid sample fabricated a measurement")
                samples_verified += len(field["samples"])
            else:
                require("samples" not in field, "Omitted field unexpectedly contains samples")
            for row in [x for x in rows if x["record_type"] == "deviation-" + direction and text(x["result"]) == value["result"]]:
                statistic = {"absolute-p95": "absoluteP95", "selected-area": "selectedArea", "evaluated-area": "evaluatedArea",
                             "out-of-tolerance-area": "outOfToleranceArea"}.get(row["metric"], row["metric"])
                if statistic in ("minimum", "maximum", "mean", "rms", "absoluteP95"):
                    number(row["measured"], field["statistics"][statistic] if field["statistics"]["hasValues"] else None)
                elif statistic in ("selectedArea", "evaluatedArea"):
                    number(row["measured"], field["statistics"][statistic])
                elif statistic == "outOfToleranceArea":
                    number(row["measured"], field["statistics"][statistic] if direction == "forward" and value["parameters"]["tolerance"] else None)
            sample_rows = [x for x in rows if x["record_type"] == "deviation-" + direction + "-sample" and text(x["result"]) == value["result"]]
            if sample_rows:
                require(field["samplesIncluded"] and len(sample_rows) == field["sampleCount"], "CSV sample coverage mismatch")
                for row in sample_rows:
                    sample = field["samples"][int(row["vertex"])]; valid = sample["state"] == "Valid"
                    number(row["measured"], sample["value"] if valid else None)
                    require(row["sample_state"] == sample["state"], "CSV sample state mismatch")
    # 按记录身份逐项对账，不允许以行数总量代替覆盖，也不接受未知行。
    def row_key(row):
        kind = row["record_type"]
        detail = text(row["item_id"]) if kind == "inspection" else row["vertex"] if kind.endswith("-sample") else row["metric"] if kind in ("deviation-forward", "deviation-reverse", "voxel-volume") else ""
        return kind, text(row["result"]), detail
    indexed = {row_key(row): row for row in rows}
    require(len(indexed) == len(rows), "Duplicate CSV row identity")
    expected = set()
    units = {"Millimeter": "mm", "Meter": "m", "ModelUnit": "model-unit"}
    def expect(kind, result_ref, owner_ref, detail="", **columns):
        key = kind, result_ref, detail
        require(key in indexed, "Missing required CSV row: " + str(key))
        expected.add(key); row = indexed[key]
        require(text(row["alignment"]) == owner_ref, "CSV result belongs to another alignment")
        for name, value in columns.items():
            require(text(row[name]) == str(value), "CSV column mismatch: " + name)
        return row
    for a in alignments.values():
        row = expect("alignment", a["result"], a["result"], metric="alignment-rms", status=a["diagnostics"]["status"],
                     value_unit=units[a["input"]["unit"]], current_at_capture=str(a["wasCurrentAtCapture"]).lower(), message=a["diagnostics"]["message"])
        number(row["measured"], a["diagnostics"]["rms"])
    for v in inspections.values():
        expect("inspection-summary", v["result"], v["alignmentResult"], verdict=v["report"]["verdict"],
               metric="inspection-verdict", current_at_capture=str(v["isCurrent"]).lower())
        for i, item in enumerate(v["report"]["items"]):
            metric = v["plan"]["items"][i]["measured"]["metric"]
            expect("inspection", v["result"], v["alignmentResult"], item["id"],
                   value_unit="deg" if metric == "OrientedDirectionAngleDegrees" else units[v["unit"]],
                   current_at_capture=str(v["isCurrent"]).lower())
    states = ["NotSelected", "Valid", "InvalidSource", "InvalidTarget", "TooFar", "NormalMismatch", "NormalUnavailable"]
    for v in deviations.values():
        expect("deviation-summary", v["result"], v["alignmentResult"], verdict=v["verdict"],
               metric="deviation-verdict", current_at_capture=str(v["isCurrent"]).lower())
        limits = v["parameters"]["tolerance"]
        if limits:
            row = expect("deviation-tolerance", v["result"], v["alignmentResult"], value_unit=units[v["unit"]], verdict=v["verdict"])
            number(row["lower_limit"], limits[0]); number(row["upper_limit"], limits[1])
        for direction in ("forward", "reverse"):
            field = v[direction]
            if field is None:
                continue
            s = field["statistics"]; unit = units[v["unit"]]
            values = {"minimum": s["minimum"], "maximum": s["maximum"], "mean": s["mean"], "rms": s["rms"], "absolute-p95": s["absoluteP95"],
                      "selected-area": s["selectedArea"], "evaluated-area": s["evaluatedArea"],
                      "out-of-tolerance-area": s["outOfToleranceArea"] if direction == "forward" and limits else None}
            for i, state in enumerate(states):
                values[state + "-count"] = s["counts"][i]; values[state + "-area"] = s["areas"][i]
            for metric, value in values.items():
                row = expect("deviation-" + direction, v["result"], v["alignmentResult"], metric,
                             value_unit="count" if metric.endswith("-count") else unit + "^2" if metric.endswith("-area") else unit,
                             verdict=v["verdict"] if direction == "forward" else "NotEvaluated", current_at_capture=str(v["isCurrent"]).lower())
                if metric.endswith("-count"):
                    require(int(row["measured"]) == value, "CSV integer count precision mismatch")
                else:
                    number(row["measured"], value)
            if fixture:  # 合成fixture要求逐点CSV；真实审计明确输出统计CSV。
                for vertex, sample in enumerate(field["samples"]):
                    row = expect("deviation-" + direction + "-sample", v["result"], v["alignmentResult"], str(vertex),
                                 sample_state=sample["state"], value_unit=unit, current_at_capture=str(v["isCurrent"]).lower())
                    valid = sample["state"] == "Valid"
                    number(row["distance"], sample["distance"] if valid else None); number(row["area"], sample["area"])
                    require(row["closest_triangle"] == (str(sample["closestTriangle"]) if valid else ""), "CSV nearest triangle mismatch")
    for v in volumes.values():
        p = v["parameters"]; st = v["statistics"]; g = v["geometry"]
        require(p["source"] == v["source"] and v["volumeUnit"] == "mm^3", "JSON voxel source or unit mismatch")
        require(st["total"] == sum(st[k] for k in ("selected", "unselected", "masked", "noData", "nonFinite", "invalidLabels")), "Voxel count conservation")
        require((st["volumeMm3"] is not None) == (st["validity"] == "Complete"), "Voxel completeness ambiguity")
        if st["volumeMm3"] is not None:
            require(st["volumeMm3"] == st["observedVolumeMm3"], "Complete voxel volume mismatch")
        metrics = {"volume": st["volumeMm3"], "observed-volume": st["observedVolumeMm3"], "cell-volume": st["cellVolumeMm3"],
                   "total": st["total"], "selected": st["selected"], "unselected": st["unselected"], "masked": st["masked"],
                   "no-data": st["noData"], "non-finite": st["nonFinite"], "invalid-labels": st["invalidLabels"]}
        for metric, value in metrics.items():
            count = metric not in ("volume", "observed-volume", "cell-volume")
            row = expect("voxel-volume", v["result"], "", metric, item_id=p["id"], status=st["validity"],
                         value_unit="count" if count else "mm^3", current_at_capture=str(v["isCurrent"]).lower(),
                         source_volume=v["source"], validity_image=p["validityImage"] or "", voxel_method=p["method"],
                         coordinate_frame=g["coordinateFrame"], model_unit="mm", algorithm_version=v["algorithmVersion"],
                         require_enclosed_selection=str(p["requireEnclosedSelection"]).lower(), touches_region_boundary=str(st["touchesRegionBoundary"]).lower())
            if count: require(int(row["measured"]) == value, "Voxel count precision lost")
            else: number(row["measured"], value)
            for col, value in (("voxel_extent",st["evaluatedExtent"]),("voxel_range",p["storedValueRange"]),("voxel_labels",p["labels"]),
                               ("voxel_spacing",g["spacing"]),("voxel_origin",g["origin"]),("voxel_direction",g["direction"])):
                require((json.loads(row[col]) if row[col] else None) == value, "CSV voxel context mismatch: " + col)
    presentation = report["presentation"]
    for display in presentation["deviations"]:
        parameters = display["parameters"]; deviation = deviations[parameters["result"]]
        row = expect("deviation-display",deviation["result"],deviation["alignmentResult"],metric="display-configuration",status=presentation["source"],current_at_capture=str(deviation["isCurrent"]).lower())
        require(json.loads(row["configuration_json"]) == display, "CSV deviation display configuration mismatch")
        require(all(row[k] == "" for k in ("measured","nominal","deviation","lower_limit","upper_limit","verdict")), "Display row fabricates a measurement")
        low, high = display["mappingRange"]; tolerance = deviation["parameters"]["tolerance"]
        def byte_color(rgb): return [math.floor(v*255+.5) for v in rgb]
        def color(value):
            if value < low: return byte_color(parameters["belowColor"])
            if value > high: return byte_color(parameters["aboveColor"])
            if parameters["useToleranceColors"] and tolerance:
                a,b=tolerance
                return byte_color(parameters["belowColor"]) if value<a else byte_color(parameters["aboveColor"]) if value>b else [60,180,95]
            f=.5 if high==low else (value-low)/(high-low)
            palette=parameters["palette"]
            # v4 的连续色带在数据范围内保留两端各 4% 的短过渡。
            if palette!=0:f=.04+.92*f
            if parameters["isReversed"]: f=1-f
            if palette==0:return byte_color(parameters["constantColor"])
            if palette==1:return byte_color([a+f*(b-a) for a,b in zip(parameters["lowColor"],parameters["highColor"])])
            if palette==5:
                a,b=([.294,.294,.84],[.88,.88,.88]) if f<.5 else ([.88,.88,.88],[.84,.294,.294])
                t=2*f if f<.5 else 2*f-1
                return byte_color([x+t*(y-x) for x,y in zip(a,b)])
            hue=(1-f)*2/3 if palette==2 else f*2/3 if palette==3 else f
            return byte_color(colorsys.hsv_to_rgb(hue,.65,.84))
        require(display["legendRgb"] == [color(low+(high-low)*(i/256)) for i in range(257)], "Independent colorbar calculation mismatch")
        require(display["colorMapVersion"] == "deviation-mapping-4" and display["invalidRgb"] == [128,128,128] and display["selectedRgb"] == [255,255,0] and not display["clippingAffectsStatistics"], "Colorbar semantics mismatch")
        selection = display["selection"]
        require((selection is not None) == (parameters["selectedVertex"] is not None), "Selection capture mismatch")
        if selection:
            require(selection["result"] == deviation["result"] and selection["vertex"] == parameters["selectedVertex"] and selection["isCurrent"] == deviation["isCurrent"], "Selected deviation identity mismatch")
            matrix = deviation["sourceToTarget"]; p = selection["sourcePoint"]
            for axis in range(3): require(math.isclose(selection["alignedPoint"][axis],sum(matrix[4*axis+j]*p[j] for j in range(3))+matrix[4*axis+3],rel_tol=1e-12,abs_tol=1e-10), "Selected point transform mismatch")
            if deviation["forward"]["samplesIncluded"]: require(selection["sample"] == deviation["forward"]["samples"][selection["vertex"]], "Selected sample is not original unclipped field data")
    scene = presentation["scene"]
    if scene:
        parameters = scene["parameters"]; anchor = alignments[parameters["anchorAlignment"]]
        row = expect("scene-display",anchor["result"],anchor["result"],metric="display-configuration",status=presentation["source"],current_at_capture=str(scene["wasCurrentAtCapture"]).lower())
        require(json.loads(row["configuration_json"]) == scene, "CSV scene configuration mismatch")
        require(all(row[k] == "" for k in ("measured","nominal","deviation","lower_limit","upper_limit","verdict")), "Scene configuration fabricates a measurement")
        inverse = scene["targetToModel"]; forward = anchor["sourceToTarget"]
        for i in range(4):
            for j in range(4): require(math.isclose(sum(inverse[4*i+k]*forward[4*k+j] for k in range(4)),int(i==j),abs_tol=1e-10), "Report scene anchor inverse mismatch")
        current = anchor["wasCurrentAtCapture"]
        for part in parameters["parts"]:
            current = current and alignments[part["alignmentResult"]]["wasCurrentAtCapture"]
            if part["inspection"]:
                inspection = inspections[part["inspection"]]; current = current and inspection["isCurrent"]
                require(inspection["alignmentResult"] == part["alignmentResult"] and (part["selectedItem"] is None or any(v["id"] == part["selectedItem"] for v in inspection["plan"]["items"])), "Scene inspection selection mismatch")
        require(current == scene["wasCurrentAtCapture"] and scene["glyphVersion"] == "inspection-glyphs-1" and not scene["glyphLengthIsMeasuredHeight"], "Scene provenance/currentness mismatch")
    require(set(indexed) == expected, "CSV contains unaccounted rows")
    if fixture:
        require(report["title"] == '报告 "A",\n二'.replace("\\n", "\n"), "Unicode/control JSON round trip failed")
        require(any(a["diagnostics"]["condition"] == {"numericState": "PositiveInfinity"} for a in alignments.values()), "Infinite diagnostic lost")
        require(report["inputs"][0]["image"]["metadata"]["scalar"]["noData"] == {"numericState": "NaN"}, "NaN metadata lost")
        require(next(iter(alignments.values()))["residuals"]["columns"][1]["values"][0] == 2**64 - 1, "UInt64 precision lost")
        require(len(volumes) == 3 and {v["statistics"]["validity"] for v in volumes.values()} == {"Complete", "TouchesBoundary", "IncompleteSamples"}, "Voxel fixture state coverage")
        require(any(v["parameters"]["labels"] == [2**53+1,2**64-1] for v in volumes.values()), "Label integer precision lost")
        require(samples_verified == 3 and all(x["report_id"].startswith("'=") for x in rows), "Fixture sample or spreadsheet text protection mismatch")
    result = {"jsonSha256": hashlib.sha256(json_path.read_bytes()).hexdigest(), "csvSha256": hashlib.sha256(csv_path.read_bytes()).hexdigest(),
              "csvRows": len(rows), "voxelVolumes": len(volumes), "displayConfigurations": len(presentation["deviations"])+int(scene is not None), "inspectionRows": len(dimension_rows), "jsonDeviationSamples": samples_verified, "passed": True}
    print("REPORT_REFERENCE " + json.dumps(result))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("json"); parser.add_argument("csv"); parser.add_argument("--fixture", action="store_true")
    args = parser.parse_args(); verify_report(Path(args.json), Path(args.csv), args.fixture)
