"""真实 600 整卷报告验收；原样回读只证明报告保真和比较契约。"""
import argparse
import json
import math
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--spacing', type=float, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--launcher', type=Path, required=True)
args = parser.parse_args()
raw = args.input.resolve(strict=True)
if raw.stat().st_size != 600 * 1800 * 600 * 4:
    parser.error('需要完整 600x1800x600 float32 RAW，不能用小样本替代')
if not math.isfinite(args.spacing) or args.spacing <= 0:
    parser.error('需要明确的正体素间距，mm')
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
launcher = args.launcher.resolve(strict=True)
identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
steps = []


def step(module, action, parameters, status='Succeeded', **extra):
    row = dict(module=module, action=action, control='actionButton', parameters=parameters,
               expectStatus=status, timeoutMs=3600000,
               reportPreview=(output / f'preview-{len(steps)}').as_posix())
    row.update(extra)
    steps.append(row)


step('Data', 'Load', dict(filePath=raw.as_posix(), dimensions=[600, 1800, 600],
     spacingLPS=[args.spacing] * 3, originLPS=[0, 0, 0], directionLPS=[1, 0, 0, 0, 1, 0, 0, 0, 1],
     datasetId='ct-600x1800x600-report-full', evidenceKind='real-data'), saveAs='loaded')
step('Surface', 'Determine', dict(initialIsoValue=.08, materialRange=None, componentSelection='All',
     analysisRoi='', minimumObjectVoxels='0'), control='businessPrimary', parameterAction='GlobalAutomatic', saveAs='surface')
step('Part', 'Start', dict(threshold='$surface.result.isoEstimate.iso'), control='businessPrimary')
step('Alignment', 'EvaluateVoxelVolume', dict(source='current', extent=None,
     storedValueRange=['$surface.result.isoEstimate.iso', '$loaded.current.scalarRange.1'], requireEnclosedSelection=False),
     status='FullyDetermined', control='businessPrimary', expectPrimaryAction='EvaluateVoxelVolume')
step('Wall', 'Start', dict(source='current', labels='parts', mesh='surface', materialLabels=[],
     maxDistance=20, evaluationRoi=''), control='businessPrimary', expectPrimaryAction='Start')
step('Gap', 'Start', dict(absoluteIsoValue='$surface.result.isoEstimate.iso', isoMode='AbsoluteValue',
     filter=False, evaluationRoi='', detectionRoi='', minVolumeMM3=0))
# 完整 CT 表面的真实几何检测；已知变换参考只用于流程，不作为独立计量真值。
step('Surface', 'SamplePoints', dict(maxPoints='4096'), status='Observed', saveAs='samples')
reference = (output / 'reference.json').as_posix()
steps.append(dict(generateKnownTransformReference=True, parameters=dict(outputPath=reference,
                  transform=identity, samples='$samples.result.samples',
                  provenance='Full real CT surface; identity-transform report workflow regression')))
step('Alignment', 'ImportReference', dict(filePath=reference, source='current', mesh='surface'), status='ReferencePublished')
step('Alignment', 'SaveRecipe', dict(recipe=None, inputRoi=''), status='FullyDetermined')
step('Alignment', 'Start', dict(initialPoses=[identity]), status='FullyDetermined')
step('Alignment', 'InspectGeometry', dict(result='current', id='full-real-surface-report', kind='Point',
     vertexIds=[], targetBounds=None, inputRoi='', targetDirection=[0, 0, 1], useSectionPlane=False,
     sectionPlaneVertexIds=[], sectionPlaneBounds=None, sectionPlaneNormal=[0, 0, 1],
     measurementMetric='CenterX', nominal=None, nominalSource='', deviationLimits=None), status='FullyDetermined')
case = output / 'case.json'
case.write_text(json.dumps(dict(schema='qt-control-scenario-1', viewsVisible=False,
                checkReports=True, checkLargeReports=True, steps=steps), ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
command = f'call "{launcher}" --background --case "{case}" --record "{output / "record.json"}"'
with (output / 'run.log').open('wb') as log:
    result = subprocess.run(command, shell=True, cwd=output, stdout=log, stderr=subprocess.STDOUT)
print(f'Real full-volume report acceptance exitCode={result.returncode}', flush=True)
raise SystemExit(result.returncode)
