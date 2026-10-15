import json
import argparse
import csv
import math
import subprocess
import time
from pathlib import Path

# 本组真实 CT 场景按 600×1800×600、float32、X 最快和 LPS 原点零设计。
# 输入只读；物理间距由调用者明确提供，输出和每次失败证据按目录隔离。
parser = argparse.ArgumentParser(description='真实 CT 多 ROI 连续业务回归')
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--spacing', type=float, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--case', action='append', default=[])
parser.add_argument('--launcher', type=Path)
args = parser.parse_args()
ROOT = args.output.resolve()
S = args.spacing
if not math.isfinite(S) or S <= 0:
    parser.error('spacing 必须是有限正数，单位 mm')
RAW = str(args.input.resolve(strict=True))
if Path(RAW).stat().st_size != 600*1800*600*4:
    parser.error('本组样本必须是 600×1800×600 float32 RAW')
written = []
I = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]

def step(module, action, parameters=None, status='Succeeded', save=None, **extra):
    value = dict(control='actionButton', module=module, action=action,
                 expectStatus=status, timeoutMs=900000)
    if parameters is not None: value['parameters'] = parameters
    if save: value['saveAs'] = save
    value.update(extra)
    return value

def start():
    return [step('Data','Load',dict(filePath=RAW,datasetId='ct-600x1800x600',
        dimensions=[600,1800,600],spacingLPS=[S]*3,originLPS=[0]*3,
        directionLPS=[1,0,0,0,1,0,0,0,1],evidenceKind='real-data'),save='loaded'),
        step('View','Set',dict(viewId='primary-3d',mode='CompositeVolume',opacity=1))]

def ref(name): return f'${name}.result.roi'
SOURCE = '$loaded.current.dataRevision'
def point(x,y,z): return [-x*S,-y*S,z*S]
def geometry(box):
    lo,hi=box
    return dict(shape='Box',centerMM=point(*[(a+b)/2 for a,b in zip(lo,hi)]),
        sizeMM=[(b-a+1)*S for a,b in zip(lo,hi)],direction3D=[1,0,0,0,1,0,0,0,1],boxToSource=None)

def roi(steps,name,box,source=SOURCE):
    steps.append(step('Roi','Begin',dict(name=name,outputMode='CreateNew',selectionMode='Replace',**geometry(box))))
    steps.append(step('Roi','Commit',save=name,expectRoiSource=source))

def select(steps,revision):
    steps.append(step('Data','Select',dict(revision=revision),expectCurrentInput=revision))

def crop(steps,roi_ref,name,source=SOURCE):
    steps.append(step('Crop','PrepareRoi',dict(inputRoi=roi_ref),status='ParametersCopied'))
    steps.append(step('Crop','FinishEditing',status='PreviewConfirmed',waitForCropFrames=True))
    steps.append(step('Crop','BuildResult',status='Published',save=name,waitForCropFrames=True,
                      expectResult={'source':source}))
    select(steps,f'${name}.result.output')

def evidence(steps,name,valid=None):
    kwargs={}
    if valid is not None: kwargs['expectMetrics']={'validCount':{'min':valid,'max':valid}}
    steps.append(step('Data','ImageEvidence',dict(revision='current',maskPath='',compareScalars=False),
                      status='Observed',save=name,**kwargs))

def write(name,steps):
    if args.case and name not in args.case: return
    folder=ROOT/name;folder.mkdir(parents=True,exist_ok=True)
    for n,s in enumerate(steps):
        if s.get('generateKnownTransformReference'): continue
        s.setdefault('stateAudit',str(folder/f'{n:03d}-{s["module"]}-{s["action"]}.json').replace('\\','/'))
        s.setdefault('reportPreview',str(folder/f'report-{n:03d}-{s["module"]}-{s["action"]}').replace('\\','/'))
    (folder/'case.json').write_text(json.dumps(dict(schema='qt-control-scenario-1',
        checkWindowStable=True,checkReportPreviews=True,viewsVisible=True,viewLayout='Inspection',steps=steps),ensure_ascii=False,indent=2),encoding='utf-8')
    written.append(name)

def operation(steps,action,target,name,**params):
    steps.append(step('Roi',action,dict(roi=target,outputMode='CreateNew',**params),status='PreviewReady'))
    steps.append(step('Roi','Commit',save=name))

def read_roi(steps,target,name,count=None):
    checks={'expectMetrics':{'voxelCount':{'min':1}}}
    if count is not None: checks={'expectResult':{'voxelCount':str(count)}}
    steps.append(step('Data','ReadRoi',dict(roi=target,maskPath=''),status='Observed',save=name,**checks))

def surface(steps,target,name):
    steps.append(step('Surface','Determine',dict(componentSelection='All',analysisRoi=target,
        initialIsoValue=0.08,materialRange=None,parentGeneration='',minimumObjectVoxels='0'),
        save=name,control='businessPrimary',parameterAction='GlobalAutomatic',expectPrimaryAction='Determine',
        expectMetrics={'points':{'min':1},'objectCount':{'min':1},'acceptedPoints':{'min':1}}))

def gap(steps,target,iso=0.08):
    steps.append(step('Gap','Start',dict(isoMode='AbsoluteValue',absoluteIsoValue=iso,filter=False,
        evaluationRoi=target,detectionRoi=target,minVolumeMM3=0),save='gap',
        expectMetrics={'evaluationVoxels':{'min':1},'actualSupportVoxels':{'min':1},'objectVoxels':{'min':1}}))
    steps.append(step('Gap','Defects',status='Observed',save='gapDetails'))

def exact(steps,shape='Box',parent='Applied',mode='KeepInside',center=None,size=20,radius=2,height=8,axis=None,save=None):
    center=center or point(189.5,339.5,299.5)
    matrix=[size*S/2,0,0,center[0],0,size*S/2,0,center[1],0,0,size*S/2,center[2],0,0,0,1]
    steps.append(step('Crop','AppendExact',dict(shape=shape,parentNodeId=parent,removalMode=mode,
        matrix=matrix,center=center,axis=axis or [0,0,1],radius=radius,height=height),save=save))

def publish(steps,name,source):
    steps.append(step('Crop','FinishEditing',status='PreviewConfirmed',waitForCropFrames=True))
    steps.append(step('Crop','BuildResult',status='Published',save=name,waitForCropFrames=True,
        expectResult={'source':source}))
    select(steps,f'${name}.result.output')

A=((170,320,280),(209,359,319))
B=((220,350,310),(259,389,349))
C=((180,400,280),(219,439,319))

steps=start()
for n,b in [('A',A),('B',B),('C',C)]: roi(steps,n,b)
for n in ['A','B','C']:
    select(steps,SOURCE)
    crop(steps,ref(n),'V'+n)
    evidence(steps,'initial'+n,40**3)
for n in ['A','B','C']:
    select(steps,f'$V{n}.result.output')
    evidence(steps,'revisited'+n,40**3)
    box={'A':A,'B':B,'C':C}[n]
    roi(steps,'new'+n,box,source=f'$V{n}.result.output')
    operation(steps,'Erode',ref('new'+n),'eroded'+n,radius=1,radiusUnit='Voxels')
    read_roi(steps,ref('eroded'+n),'check'+n)
    surface(steps,ref('eroded'+n),'surface'+n)
write('05-independent-crops-complete',steps)

steps=start();roi(steps,'A',A)
crop(steps,ref('A'),'V1');evidence(steps,'first',40**3)
select(steps,SOURCE)
steps.append(step('Roi','Edit',{'roi':ref('A')}))
steps.append(step('Roi','SetDraft',dict(name='A-second',outputMode='UpdateTarget',**geometry(((170,320,280),(219,369,329))))))
steps.append(step('Roi','Commit',save='A2',expectRoiSource=SOURCE))
crop(steps,ref('A2'),'V2');evidence(steps,'second',50**3)
select(steps,SOURCE)
crop(steps,ref('A'),'V1again');evidence(steps,'firstAgain',40**3)
write('02-roi-revisions',steps)

# 三个分量的布尔/形态/拆分/部分合并链。
steps=start()
roi(steps,'A',((170,320,280),(199,359,319)))
roi(steps,'B',((210,320,280),(249,359,319)))
roi(steps,'C',((229,300,260),(238,380,340)))
operation(steps,'Union',ref('A'),'D',operands=[ref('B')])
read_roi(steps,ref('D'),'Dmask',70*40*40)
operation(steps,'Difference',ref('D'),'E',operands=[ref('C')])
read_roi(steps,ref('E'),'Emask',60*40*40)
operation(steps,'Dilate',ref('E'),'expanded',radius=1,radiusUnit='Voxels')
operation(steps,'Close',ref('expanded'),'closed',radius=1,radiusUnit='Voxels')
steps.append(step('Roi','Split',dict(roi=ref('closed'),outputMode='CreateNew',connectivity='Faces',keepInput=True),status='PreviewReady',expectResult={'outputCount':3}))
steps.append(step('Roi','Commit',save='split'))
steps.append(step('Data','RoiDescriptors',{'namePrefix':'A /'},status='Observed',save='catalog',expectResult={'count':3}))
operation(steps,'Merge','$catalog.result.rois.0.roi','merged',operands=['$catalog.result.rois.1.roi'])
read_roi(steps,ref('merged'),'mergedMask')
surface(steps,ref('merged'),'surface')
gap(steps,ref('merged'))
write('00-multi-roi-operations',steps)

steps=start()
for n,b in [('A',A),('B',((190,330,290),(229,369,329))),('C',((180,310,270),(219,349,309)))]:roi(steps,n,b)
steps += [step('Roi','Edit',{'roi':ref('A')}),step('Roi','SetDraft',dict(name='A-revised',outputMode='UpdateTarget',**geometry(((175,320,280),(214,359,319))))),step('Roi','Commit',save='A2')]
operation(steps,'Erode',ref('B'),'B2',radius=1,radiusUnit='Voxels')
operation(steps,'Difference',ref('C'),'C2',operands=[ref('B2')])
operation(steps,'Intersection',ref('A2'),'A3',operands=[ref('B2')])
read_roi(steps,ref('A3'),'intersection')
crop(steps,ref('C2'),'VC');evidence(steps,'Coutput')
select(steps,SOURCE);crop(steps,ref('A3'),'VA');evidence(steps,'Aoutput')
write('01-alternating-rois',steps)

steps=start();roi(steps,'A',A);crop(steps,ref('A'),'V1')
steps.append(step('Crop','Start',dict(shape='Box',removalMode='KeepInside')))
exact(steps,parent='Root',size=30,save='firstGeometry')
publish(steps,'V2','$V1.result.output')
steps.append(step('Crop','Start',dict(shape='Box',removalMode='KeepInside')))
exact(steps,parent='Root',size=30,save='sameGeometry')
publish(steps,'V3','$V2.result.output')
evidence(steps,'repeatedCrop',30**3)
roi(steps,'newRoi',((180,330,290),(199,349,309)),source='$V3.result.output')
operation(steps,'Dilate',ref('newRoi'),'newExpanded',radius=1,radiusUnit='Voxels')
gap(steps,ref('newExpanded'))
write('03-repeat-same-crop',steps)

steps=start();roi(steps,'A',A);crop(steps,ref('A'),'V1')
steps.append(step('Crop','Start',dict(shape='Box',removalMode='KeepInside')))
exact(steps,parent='Root',size=36,save='box')
exact(steps,'Sphere',mode='RemoveInside',radius=0.5,save='sphere')
exact(steps,'Plane',center=point(190,340,292),axis=[0,0,1],save='plane')
exact(steps,'Cylinder',mode='RemoveInside',center=point(182,340,300),radius=0.4,height=8,save='cylinder')
publish(steps,'V2','$V1.result.output');evidence(steps,'mixedCrop')
roi(steps,'newRoi',((180,330,294),(199,349,309)),source='$V2.result.output')
surface(steps,ref('newRoi'),'surface')
write('04-mixed-shape-crop',steps)

steps=start();source=SOURCE
for layer,names in enumerate([('A','B'),('C','D'),('E','F')],1):
    for offset,n in enumerate(names):roi(steps,n,((175+offset*5,325+offset*5,285),(204+offset*5,354+offset*5,314)),source=source)
    operation(steps,'Intersection',ref(names[0]),f'combined{layer}',operands=[ref(names[1])])
    crop(steps,ref(f'combined{layer}'),f'V{layer}',source=source)
    evidence(steps,f'evidence{layer}',25*25*30)
    source=f'$V{layer}.result.output'
surface(steps,'','surface')
write('06-multi-level-loop',steps)

steps=start()
roi(steps,'B',((185,325,285),(214,354,314)))
roi(steps,'C',((190,330,290),(194,349,309)))
steps.append(step('Crop','Start',dict(shape='Box',removalMode='KeepInside')))
exact(steps,parent='Root',size=38)
exact(steps,'Sphere',mode='RemoveInside',radius=0.3)
steps.append(step('Crop','SaveRoi',{'name':'preview-range'},save='savedRoi'))
read_roi(steps,'$savedRoi.result.recipe','savedRange')
operation(steps,'Union','$savedRange.result.roi','rangeUnion',operands=[ref('B')])
operation(steps,'Difference',ref('rangeUnion'),'D',operands=[ref('C')])
crop(steps,ref('D'),'V1')
roi(steps,'afterRange',((180,330,290),(199,349,309)),source='$V1.result.output')
surface(steps,ref('afterRange'),'surface')
write('07-preview-to-roi',steps)

steps=start();steps.append(step('Crop','Start',dict(shape='Box',removalMode='KeepInside')))
exact(steps,parent='Root',size=40,save='N1')
exact(steps,'Sphere',mode='RemoveInside',radius=0.4,save='N2')
exact(steps,'Plane',center=point(190,340,290),save='N3')
publish(steps,'branch1',SOURCE)
select(steps,SOURCE)
steps.append(step('Crop','Node',{'nodeId':'$N1.result.nodeId'},control='catalogAction',waitForCropFrames=True))
exact(steps,'Cylinder',mode='RemoveInside',radius=0.5,height=10,save='N4')
publish(steps,'branch2',SOURCE)
for n in [1,2]:
    select(steps,f'$branch{n}.result.output')
    roi(steps,f'roiBranch{n}',((180,330,294),(199,349,309)),source=f'$branch{n}.result.output')
    surface(steps,ref(f'roiBranch{n}'),f'surfaceBranch{n}')
select(steps,SOURCE)
steps.append(step('Crop','Node',{'nodeId':'$N3.result.nodeId'},control='catalogAction',waitForCropFrames=True))
exact(steps,'Sphere',mode='RemoveInside',radius=0.25,center=point(184,340,300),save='oldBranchContinued')
publish(steps,'branch1continued',SOURCE)
write('08-crop-history-branches',steps)

def wall(steps,roi_ref,labels,mesh,name,source):
    steps.append(step('Wall','Start',dict(source=source,labels=labels,mesh=mesh,materialLabels=[],
        unit='Millimeter',materialThreshold=0.08,maxDistance=12,sampleSpacing=None,
        coneAngleDegrees=30,directionCount=9,boundaryPolicy='SourceExtentLocal',maxBoundaryError=None,
        evaluationRoi=roi_ref),save=name,expectResult={'source':source,'labels':labels,'mesh':mesh},
        expectMetrics={'sampleCount':{'min':1},'validCount':{'min':1}},timeoutMs=3600000))

steps=start();roi(steps,'A',A);roi(steps,'B',((190,330,290),(229,369,329)))
operation(steps,'Union',ref('A'),'D',operands=[ref('B')]);crop(steps,ref('D'),'V1')
newsource='$V1.result.output'
roi(steps,'localA',A,source=newsource)
roi(steps,'localB',((190,330,290),(229,369,329)),source=newsource)
surface(steps,'','surface')
steps.append(step('Part','Start',dict(threshold=0.08,minPartVoxels='1'),save='parts'))
steps.append(step('Part','Catalog',status='Observed',save='partsCatalog'))
steps.append(step('Part','Erase',dict(target='$partsCatalog.result.parts.0.binding',editRoi=ref('localA'),
    sourcePointsMM=[point(189,355,300)],radiusMM=0.4),status='PreviewReady',save='editA'))
steps.append(step('Part','Commit',dict(previewId='current'),save='labelsA'))
steps.append(step('Data','LabelDescriptors',status='Observed',save='labelIds'))
steps.append(step('Data','ReadLabelRegion',dict(id='$labelIds.result.labels.0.id',revision='$labelsA.result.labelMap',
    offset=[410,1444,300],size=[1,1,1]),expectResult={'nativeBytesHex':'00000000'}))
steps.append(step('Data','ReadLabelRegion',dict(id='$labelIds.result.labels.0.id',revision='$labelsA.result.labelMap',
    offset=[384,1449,310],size=[1,1,1]),save='beforeB',expectResult={'nativeBytesHex':'01000000'}))
steps.append(step('Part','Catalog',status='Observed',save='partsCatalogB'))
steps.append(step('Part','Erase',dict(target='$partsCatalogB.result.parts.0.binding',editRoi=ref('localB'),
    sourcePointsMM=[point(215,350,310)],radiusMM=0.4),status='PreviewReady',save='editB'))
steps.append(step('Part','Commit',dict(previewId='current'),save='labelsB'))
steps.append(step('Data','ReadLabelRegion',dict(id='$labelIds.result.labels.0.id',revision='$labelsB.result.labelMap',
    offset=[384,1449,310],size=[1,1,1]),expectResult={'nativeBytesHex':'00000000'}))
wall(steps,ref('localA'),'$labelsB.result.labelMap','$surface.result.mesh','wallA',newsource)
operation(steps,'Erode',ref('localA'),'localA2',radius=1,radiusUnit='Voxels')
steps.append(step('Part','Undo',status='PreviewReady',save='undoPreview'))
steps.append(step('Part','Commit',dict(previewId='current'),save='labelsUndo'))
steps.append(step('Data','ReadLabelRegion',dict(id='$labelIds.result.labels.0.id',revision='$labelsUndo.result.labelMap',
    offset=[384,1449,310],size=[1,1,1]),expectResult={'nativeBytesHex':'$beforeB.result.nativeBytesHex'}))
wall(steps,ref('localA2'),'$labelsUndo.result.labelMap','$surface.result.mesh','wallB',newsource)
write('09-label-edit-wall-repeat',steps)

steps=start();roi(steps,'A',A);roi(steps,'B',((190,330,290),(229,369,329)))
operation(steps,'Union',ref('A'),'D',operands=[ref('B')]);crop(steps,ref('D'),'V1')
source='$V1.result.output'
evidence(steps,'beforeArtifact',110000)
roi(steps,'poreRoi',((175,325,285),(204,354,314)),source=source);gap(steps,ref('poreRoi'))
roi(steps,'artifactRoi',((180,330,290),(199,349,309)),source=source)
steps.append(step('Artifact','Diffusion',dict(source=source,processingRoi=ref('artifactRoi'),
    protectionRoi='',qualityRoi='',diffusion=dict(iterations=1,threshold=1,factor=0.25,slabDepth=8)),status='Ready',save='artifact',
    expectMetrics={'quality.changedCount':{'min':1},'quality.validCount':{'min':110000,'max':110000}}))
steps.append(step('Artifact','Commit',status='Published',save='corrected'))
select(steps,'$corrected.result.correctedVolume')
evidence(steps,'afterArtifact',110000)
roi(steps,'afterArtifact',((175,325,285),(204,354,314)),source='$corrected.result.correctedVolume')
operation(steps,'Erode',ref('afterArtifact'),'afterErode',radius=1,radiusUnit='Voxels')
crop(steps,ref('afterErode'),'V2',source='$corrected.result.correctedVolume');surface(steps,'','newSurface')
select(steps,source);read_roi(steps,ref('poreRoi'),'oldRoi')
surface(steps,ref('poreRoi'),'oldSurface');gap(steps,ref('poreRoi'))
write('11-interleaved-old-chain',steps)

# 正式测量表面自身的已知刚体变换参考，只验证流程和几何回归。
steps=start();roi(steps,'A',A);roi(steps,'B',((190,330,290),(229,369,329)))
operation(steps,'Union',ref('A'),'D',operands=[ref('B')]);surface(steps,ref('D'),'surface0')
operation(steps,'SurfaceLayer','','shell',mesh='$surface0.result.selectionMesh',meshToSource=I,
          inwardMM=2*S,outwardMM=2*S,hasOrientation=False)
operation(steps,'Intersection',ref('shell'),'local',operands=[ref('D')])
transform=I.copy();transform[3]=0.2;transform[7]=-0.1;transform[11]=0.05
folder=ROOT/'10-surface-metrology-cycle'
for round_no in [1,2]:
    roi_ref=ref('local')
    if round_no==2:
        operation(steps,'Dilate',ref('local'),'local2',radius=1,radiusUnit='Voxels')
        roi_ref=ref('local2')
    surface(steps,roi_ref,f'surface{round_no}')
    steps.append(step('Surface','SamplePoints',{'maxPoints':'4096'},status='Observed',save=f'samples{round_no}'))
    json_path=str(folder/f'reference-{round_no}.json').replace('\\','/')
    stl_path=str(folder/f'nominal-{round_no}.stl').replace('\\','/')
    steps.append(dict(generateKnownTransformReference=True,parameters=dict(outputPath=json_path,
        transform=transform,samples=f'$samples{round_no}.result.samples',
        provenance='Real CT formal surface with a known rigid transform; flow regression')))
    steps.append(step('Surface','ExportNominalMeshFixture',dict(outputPath=stl_path,transform=transform),status='Exported'))
    steps.append(step('Alignment','ImportReference',dict(filePath=json_path,source='current',mesh='surface'),status='ReferencePublished'))
    steps.append(step('Alignment','ImportNominalMesh',dict(filePath=stl_path,frameId='nominal-frame',coordinateFrame='RAS',fileUnit='Millimeter',unit='Millimeter'),status='FullyDetermined'))
    steps.append(step('Alignment','SaveRecipe',dict(recipe=None,inputRoi=''),status='FullyDetermined'))
    steps.append(step('Alignment','Start',dict(initialPoses=[I]),status='FullyDetermined',
        save=f'alignment{round_no}',expectedTransform=transform))
    steps.append(step('Alignment','EvaluateSurface',dict(result='current',computeBackend='Cpu',
        metric='UnsignedDistance',evaluationRoi=roi_ref,maxDistance=1,reverseCoverage=True),status='FullyDetermined'))
    # STL 导入的单精度坐标量化界：16 * sqrt(3) * epsilon(float) * 最大坐标绝对值。
    quantization_limit=16*(3**0.5)*(2**-23)*(1800*S+1)
    steps.append(step('Alignment','DeviationResult',dict(deviation='current'),status='Observed',
        save=f'deviation{round_no}',expectMetrics={'forward.maximum':{'min':0,'max':quantization_limit}}))
write('10-surface-metrology-cycle',steps)

if set(args.case) - set(written):
    parser.error('未知场景: ' + ', '.join(sorted(set(args.case) - set(written))))
if args.launcher:
    launcher = args.launcher.resolve(strict=True)
    failed = False
    for name in written:
        folder = ROOT / name
        attempt = 1
        while (folder / f'attempt-{attempt:02}').exists(): attempt += 1
        output = folder / f'attempt-{attempt:02}'
        output.mkdir()
        case_path = output / 'case.json'
        case_path.write_text((folder/'case.json').read_text(encoding='utf-8').replace(
            folder.as_posix(),output.as_posix()),encoding='utf-8')
        record_path = output / 'record.json'
        started = time.monotonic()
        # 所有 shell 参数都是已解析的本地路径；RAW 参数位于 JSON 中，不拼接进命令。
        command = f'call "{launcher}" --background --case "{case_path}" --record "{record_path}"'
        print(f'START {name} attempt={attempt}',flush=True)
        with (output/'run.log').open('wb') as log:
            result = subprocess.run(command,shell=True,stdout=log,stderr=subprocess.STDOUT,cwd=output)
        operations = 0
        if record_path.exists():
            operations = len(json.loads(record_path.read_text(encoding='utf-8-sig'))['records'])
        row = dict(case=name,attempt=attempt,status='PASS' if result.returncode == 0 else 'FAIL',
                   exitCode=result.returncode,operations=operations,
                   elapsedSeconds=round(time.monotonic()-started,2))
        summary = ROOT/'results.csv'
        needs_header = not summary.exists()
        with summary.open('a',newline='',encoding='utf-8') as stream:
            writer = csv.DictWriter(stream,fieldnames=list(row))
            if needs_header: writer.writeheader()
            writer.writerow(row)
        print(json.dumps(row,ensure_ascii=False),flush=True)
        failed = failed or result.returncode != 0
    raise SystemExit(1 if failed else 0)
