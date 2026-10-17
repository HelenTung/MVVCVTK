"""固定真实64³样本的非空孔隙、有效域边界、标签及完整CSV回归。"""
import argparse
from collections import deque
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

# 优化模式会删除验收断言，必须在任何业务启动前明确拒绝。
if sys.flags.optimize:
    raise RuntimeError('真实边界回归不能在 Python 优化模式下运行')

SIDE = 64
COUNT = SIDE ** 3
SPACING = .1537
ISO = -.46101090982812137
RAW_SHA256 = 'a4394c9de11c80c06aa1d9e230ee580c8e1371905873eb68a21e53b7105568f7'
ORIGIN = [-122.8063, -96.9847, 113.1232]
# bridge 的体积字段来自 binary32；这个舍入界不是外部物理精度门限。
VOLUME_REL_TOL = 4 * 2 ** -23


def index(x, y, z):
    return x + SIDE * (y + SIDE * z)


def neighbors(i):
    x, y, z = i % SIDE, i // SIDE % SIDE, i // SIDE ** 2
    for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
        a, b, c = x + dx, y + dy, z + dz
        yield index(a, b, c) if all(0 <= v < SIDE for v in (a, b, c)) else None


def fixture(raw):
    data = raw.read_bytes()
    assert len(data) == COUNT * 4 and hashlib.sha256(data).hexdigest() == RAW_SHA256, '需要指定的真实64³归档样本'
    stored = struct.unpack(f'<{COUNT}f', data)
    # Host 将 LPS 的 X/Y 反排成规范化 RAS，不能直接把 RAW 存储索引当作标签索引。
    values = [stored[index(SIDE - 1 - i % SIDE, SIDE - 1 - i // SIDE % SIDE, i // SIDE ** 2)] for i in range(COUNT)]
    pore, removed = index(49, 21, 34), index(50, 21, 34)
    assert values[pore] < ISO and all(values[j] >= ISO for j in neighbors(pore))
    mask = bytearray(COUNT)
    for z in range(32, 37):
        for y in range(19, 24):
            for x in range(47, 52):
                mask[index(x, y, z)] = 1
    mask[removed] = 0
    return values, mask


def components(values, mask):
    remaining = {i for i in range(COUNT) if mask[i] and values[i] < ISO}
    groups = []
    while remaining:
        seed = min(remaining)
        remaining.remove(seed)
        group, queue, boundary = {seed}, deque([seed]), False
        while queue:
            for j in neighbors(queue.popleft()):
                if j is None or not mask[j]:
                    boundary = True
                elif j in remaining:
                    remaining.remove(j)
                    group.add(j)
                    queue.append(j)
        groups.append((group, boundary))
    return groups


def make_case(raw, folder):
    steps = []

    def step(module, action, parameters=None, status='Succeeded', save=None, **extra):
        value = dict(control='actionButton', module=module, action=action, parameters=parameters or {},
                     expectStatus=status, timeoutMs=0, deferReportCheck=True)
        if save:
            value['saveAs'] = save
        value.update(extra)
        steps.append(value)
        return value

    def gap(name, formal, voxels, boundary, boundary_voxels):
        value = step('Gap', 'Start', dict(isoMode='AbsoluteValue', absoluteIsoValue=ISO,
                     backgroundMean=-.78244800304597073, materialMean=-.13957381661027202,
                     filter=False, minVolumeMM3=0), save=name,
                     reportPreview=(folder / 'previews' / name).as_posix(),
                     businessCapture=(folder / name).as_posix(), checkSceneConsistency=True,
                     stageLabel=name, expectMetrics={key: dict(min=count, max=count) for key, count in
                         [('voidCount', formal), ('voidVoxels', voxels),
                          ('boundaryUncertainCount', boundary), ('boundaryUncertainVoxels', boundary_voxels)]})
        value.pop('deferReportCheck')

    def labels(name):
        step('Data', 'LabelDescriptors', status='Observed', save=name)
        step('Data', 'ReadLabelRegion', dict(id='GapAnalysis.labels',
             revision=f'${name}.result.labels.0.revision', offset=[0, 0, 0], size=[SIDE] * 3))

    step('Data', 'Load', dict(filePath=raw.as_posix(), datasetId='real64-gap-boundary', dimensions=[SIDE] * 3,
         spacingLPS=[SPACING] * 3, originLPS=[113.1232, 87.30160000000001, 113.1232],
         directionLPS=[1, 0, 0, 0, 1, 0, 0, 0, 1], sourceDigest=RAW_SHA256, evidenceKind='archived-real-ct-roi'),
         checkSceneConsistency=True, businessCapture=(folder / 'loaded').as_posix(), stageLabel='real64 loaded four views')
    step('View', 'Set', dict(viewId='primary-3d', mode='CompositeVolume'))
    gap('initial-gap', 4, 7, 0, 0)
    labels('initialLabels')
    center = [ORIGIN[a] + [49, 21, 34][a] * SPACING for a in range(3)]
    excluded = [center[0] + SPACING, center[1], center[2]]
    half = 2.5 * SPACING
    step('Crop', 'Start', dict(shape='Box', removalMode='KeepInside'), waitForCropFrames=True)
    step('Crop', 'AppendExact', dict(parentNodeId='Root', shape='Box', removalMode='KeepInside',
         matrix=[half, 0, 0, center[0], 0, half, 0, center[1], 0, 0, half, center[2], 0, 0, 0, 1],
         center=center), waitForCropFrames=True)
    step('Crop', 'AppendExact', dict(parentNodeId='Applied', shape='Sphere', removalMode='RemoveInside',
         center=excluded, radius=.39 * SPACING), waitForCropFrames=True)
    step('Crop', 'FinishEditing', status='PreviewConfirmed', waitForCropFrames=True)
    step('Crop', 'BuildResult', status='Published', save='cropped', waitForCropFrames=True)
    step('Data', 'Select', dict(revision='$cropped.result.output', expectedBindingRevision='current'), checkSceneConsistency=True)
    step('Data', 'ImageEvidence', dict(revision='current', maskPath=(folder / 'validity.mask').as_posix(), compareScalars=False),
         status='Observed', expectMetrics={'validCount': dict(min=124, max=124), 'gridCount': dict(min=COUNT, max=COUNT)})
    # 此真实范围还有一个15体素裁切边缘分量；不能把目标单体素孔当成唯一可见对象。
    gap('boundary-gap', 0, 0, 2, 16)
    labels('boundaryLabels')
    step('Gap', 'Defects', dict(pageOffset='0'), status='Observed')
    step('Gap', 'Exit', status='Exited')
    return dict(checkReports=True, checkIdle=True, shutdownTimeoutMs=0, windowSize=[1440, 1000], steps=steps)


def audit(folder, values, expected_mask):
    record = json.loads((folder / 'record.json').read_text(encoding='utf-8-sig'))
    operations = record['records']
    assert len({r['operationId'] for r in operations}) == len(operations)
    assert all(r['isTerminal'] and r['completeCount'] == 1 for r in operations)
    assert record['shutdown']['isStopped'], 'Stop 必须实际完成，允许按现有契约重试'
    actual_mask = (folder / 'validity.mask').read_bytes()
    assert bytes(bool(v) for v in actual_mask) == expected_mask, '按非零有效语义核对完整掩膜，不假定字节为1'
    initial = [g for g, boundary in components(values, bytes([1]) * COUNT) if not boundary]
    boundary = components(values, expected_mask)
    assert sorted(map(len, initial)) == [1, 1, 1, 4]
    assert sorted(len(g) for g, _ in boundary) == [1, 15] and all(b for _, b in boundary)
    assert next(g for g, _ in boundary if index(49, 21, 34) in g) == {index(49, 21, 34)}
    starts = [r for r in operations if r['module'] == 'Gap' and r['action'] == 'Start']
    descriptors = [r for r in operations if r['module'] == 'Data' and r['action'] == 'LabelDescriptors']
    reads = [r for r in operations if r['module'] == 'Data' and r['action'] == 'ReadLabelRegion']
    assert len(starts) == len(descriptors) == len(reads) == 2
    loaded = json.loads((folder / 'loaded' / 'capture.json').read_text(encoding='utf-8-sig'))
    assert len(loaded['scenes']) == len(loaded['views']) == 4
    assert loaded['expectedDataRevision'] == starts[0]['result']['source']
    assert all(s['source'] == loaded['expectedDataRevision'] and s['bindingRevision'] == loaded['expectedBindingRevision'] for s in loaded['scenes'])
    for stage, name in enumerate(('initial-gap', 'boundary-gap')):
        core = starts[stage]['result']
        descriptor = [d for d in descriptors[stage]['result']['labels'] if d['id'] == 'GapAnalysis.labels']
        assert len(descriptor) == 1 and descriptor[0]['source'] == core['source'] and descriptor[0]['dims'] == [SIDE] * 3
        assert reads[stage]['parameters']['revision'] == descriptor[0]['revision']
        native = struct.unpack(f'<{COUNT}i', bytes.fromhex(reads[stage]['result']['nativeBytesHex']))
        by_id = {}
        for i, label in enumerate(native):
            assert label >= 0
            if label:
                by_id.setdefault(label, set()).add(i)
        expected = initial if stage == 0 else [g for g, _ in boundary]
        assert {frozenset(g) for g in by_id.values()} == {frozenset(g) for g in expected}, '标签逐体素集合必须等于独立六邻接结果'
        index_json = json.loads((folder / 'previews' / (name + '-reports.json')).read_text(encoding='utf-8-sig'))
        assert len(index_json['reports']) == 1
        report = index_json['reports'][0]
        assert report['source'] == core['source'] and report['result'] == core['resultSet']
        assert report['csvReadbackMatches'] and report['fullTableSha256'] == report['csvReadbackSha256']
        assert report['comparisonPerformed'] and report['comparisonMatches']
        with Path(report['csv']).open(encoding='utf-8-sig', newline='') as stream:
            rows = list(csv.reader(stream))
        assert len(rows) == len(by_id) + 1 and all(len(row) == 9 for row in rows)
        assert rows[0][0] == f'void_id [mvvcvtk-result={core["resultSet"]}]'
        assert len({row[0] for row in rows[1:]}) == len(by_id) and {int(row[0]) for row in rows[1:]} == set(by_id)
        for row in rows[1:]:
            voxels = by_id[int(row[0])]
            uncertain = stage == 1 and any(j is None or not actual_mask[j] for i in voxels for j in neighbors(i))
            assert row[8] == ('BoundaryUncertain' if uncertain else 'Interior')
            spacing = starts[stage]['current']['spacingRAS']
            assert spacing == [SPACING] * 3
            intersection = float(row[2])
            assert math.isfinite(intersection) and intersection == len(voxels) * (spacing[0] * spacing[1] * spacing[2]), 'CSV相交体积必须等于独立标签集合计数乘实际体素体积'
            assert math.isclose(float(row[7]), len(voxels) * SPACING ** 3, rel_tol=VOLUME_REL_TOL)
            if uncertain:
                assert row[1] == row[3] == row[4] == '', '边界对象正式/完整量值留空'
        capture = json.loads((folder / name / 'capture.json').read_text(encoding='utf-8-sig'))
        assert len(capture['scenes']) == 4 and len(capture['views']) == 4
        assert capture['expectedDataRevision'] == core['source'] == index_json['selectedDataRevision']
        assert capture['expectedBindingRevision'] == index_json['selectedBindingRevision']
        assert all(s['source'] == core['source'] and s['bindingRevision'] == capture['expectedBindingRevision'] for s in capture['scenes'])
        for image in capture['views']:
            assert (folder / name / image['file']).is_file()
        assert int(core['voidCount']) == (4 if stage == 0 else 0) and int(core['voidVoxels']) == (7 if stage == 0 else 0)
        assert int(core['boundaryUncertainCount']) == (0 if stage == 0 else 2)
        assert int(core['boundaryUncertainVoxels']) == (0 if stage == 0 else 16)
        if stage:
            assert core['voidVolumeMM3'] == 0 and core['porosityRatio'] == 0
        print(f'PASS {name}: IDs={sorted(by_id)}, voxels={sorted(map(len, by_id.values()))}, complete CSV/native-table hash={report["fullTableSha256"]}', flush=True)
    formal = [r for r in operations if r['module'] == 'Gap' and r['action'] == 'Defects'][-1]
    assert not formal['result']['page']['rows']
    assert record['hostDiagnostics']['pendingUpdateFailures'] == '0'
    print(f'PASS real64 boundary: {len(operations)} terminal operations, Stop={record["shutdown"]}; failure-notification count is not the entire update/render queue', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--launcher', type=Path, required=True)
    args = parser.parse_args()
    raw, launcher = args.input.resolve(strict=True), args.launcher.resolve(strict=True)
    values, mask = fixture(raw)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    number = 1
    while (root / f'attempt-{number:02d}').exists():
        number += 1
    folder = root / f'attempt-{number:02d}'
    folder.mkdir()
    case = folder / 'case.json'
    case.write_text(json.dumps(make_case(raw, folder), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temp = folder / 'temp'
    temp.mkdir()
    environment = os.environ.copy()
    environment.update(TEMP=str(temp), TMP=str(temp), CUDA_CACHE_PATH=str(temp / 'cuda-cache'), QT_DISABLE_SHADER_DISK_CACHE='1')
    (temp / 'cuda-cache').mkdir()
    command = f'call "{launcher}" --background --case "{case}" --record "{folder / "record.json"}"'
    print(f'RUN {command}', flush=True)
    # 与现有真实回归一致：健康任务自然完成，不以固定总期限中止业务。
    with (folder / 'run.log').open('xb') as log:
        result = subprocess.run(command, shell=True, cwd=folder, env=environment, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        return result.returncode
    audit(folder, values, mask)
    assert hashlib.sha256(raw.read_bytes()).hexdigest() == RAW_SHA256
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
