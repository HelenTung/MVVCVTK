#include "Render/CropShaderController.h"

#include "Algorithms/CropAlgorithm.h"

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkObject.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkOpenGLShaderCache.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkShader.h>
#include <vtkShaderProgram.h>
#include <vtkShaderProperty.h>
#include <vtkSmartPointer.h>
#include <vtkTextureObject.h>
#include <vtkType.h>
#include <vtkWeakPointer.h>
#define GLAD_API_CALL_EXPORT
#include <vtk_glad.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
constexpr const char* kPredicateCode = R"glsl(
bool mvvcvtkCropKept(vec3 pointMC)
{
  for (int cropIndex = 0; cropIndex < mvvcvtk_cropNodeCount; ++cropIndex) {
    int cropBase = cropIndex * 5;
    vec4 cropMeta = texelFetch(mvvcvtk_cropTable, cropBase, 0);
    bool cropInside = false;
    if (int(cropMeta.x + 0.5) == 0) {
      vec4 cropPoint = vec4(pointMC, 1.0);
      vec4 cropBox = vec4(
        dot(texelFetch(mvvcvtk_cropTable, cropBase + 1, 0), cropPoint),
        dot(texelFetch(mvvcvtk_cropTable, cropBase + 2, 0), cropPoint),
        dot(texelFetch(mvvcvtk_cropTable, cropBase + 3, 0), cropPoint),
        dot(texelFetch(mvvcvtk_cropTable, cropBase + 4, 0), cropPoint));
      cropInside = all(lessThanEqual(abs(cropBox.xyz), vec3(1.000001)));
    } else {
      vec3 cropCenter = texelFetch(mvvcvtk_cropTable, cropBase + 1, 0).xyz;
      vec3 cropNormal = texelFetch(mvvcvtk_cropTable, cropBase + 2, 0).xyz;
      cropInside = dot(pointMC - cropCenter, cropNormal) > 0.0;
    }
    bool cropKept = int(cropMeta.y + 0.5) == 0 ? cropInside : !cropInside;
    if (!cropKept) { return false; }
  }
  return true;
}
)glsl";

std::string GetPolyDec(const char* marker, const bool hasMatrix)
{
    std::string code(marker);
    code += "\nuniform sampler1D mvvcvtk_cropTable;\nuniform int mvvcvtk_cropNodeCount;\n";
    if (hasMatrix) {
        code += "uniform mat4 mvvcvtk_localToInput;\n";
    }
    code += "in vec3 mvvcvtk_cropPointMC;\n";
    code += kPredicateCode;
    return code;
}
}

class CropShaderController::Impl final {
public:
    struct Resource final {
        CropShaderPayload payload;
        vtkSmartPointer<vtkTextureObject> texture;
    };

    explicit Impl(const RenderTargetKind targetKind)
        : m_targetKind(targetKind)
    {
        m_localToInput = {
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0
        };
    }

    ~Impl()
    {
        if (m_mapper && m_observerTag != 0) {
            m_mapper->RemoveObserver(m_observerTag);
        }
        if (auto* context = m_context.GetPointer()) {
            context->MakeCurrent();
            if (m_active.texture) {
                m_active.texture->ReleaseGraphicsResources(context);
            }
            if (m_previous.texture
                && m_previous.texture != m_active.texture) {
                m_previous.texture->ReleaseGraphicsResources(context);
            }
            if (m_staged.texture && m_staged.texture != m_active.texture) {
                m_staged.texture->ReleaseGraphicsResources(context);
            }
            ClearDeferred(context);
        }
    }

    bool SetShaderTarget(vtkObject* mapper, vtkShaderProperty* shaderProperty);
    bool SetCropParams(CropShaderPayload payload);
    RenderEffectState GetState() const { return m_state; }
    bool SetCropCommit(std::uint64_t revision);
    bool GetCropCommitReady(std::uint64_t revision) const;
    bool SetCropComplete(std::uint64_t revision);
    bool ClearCropCommit(std::uint64_t revision);
    bool ClearCropStage(std::uint64_t revision);
    bool ClearCropParams();
    bool SetLocalToInput(const std::array<double, 16>& localToInput);
    bool StartRender(vtkRenderer* renderer);
    bool StopRender();

private:
    static void OnShader(vtkObject*, unsigned long, void* clientData, void* callData);
    void SetProgram(vtkShaderProgram* program);
    bool BuildTexture(vtkOpenGLRenderWindow* context);
    void ClearDeferred(vtkOpenGLRenderWindow* context);

    RenderTargetKind m_targetKind;
    RenderEffectState m_state;
    Resource m_previous;
    Resource m_active;
    Resource m_staged;
    std::uint64_t m_commitRevision = 0;
    std::array<double, 16> m_localToInput = {};
    vtkWeakPointer<vtkObject> m_mapper;
    unsigned long m_observerTag = 0;
    vtkSmartPointer<vtkCallbackCommand> m_observer;
    vtkWeakPointer<vtkOpenGLRenderWindow> m_context;
    vtkWeakPointer<vtkShaderProgram> m_program;
    std::vector<vtkSmartPointer<vtkTextureObject>> m_deferred;
    vtkSmartPointer<vtkTextureObject> m_boundTexture;
    bool m_isActive = false;
    bool m_hasProgramSync = false;
    int m_stageRenderCount = 0;
};

bool CropShaderController::Impl::SetShaderTarget(
    vtkObject* mapper,
    vtkShaderProperty* shaderProperty)
{
    if (!mapper || !shaderProperty || m_mapper) {
        return false;
    }

    if (m_targetKind == RenderTargetKind::Volume) {
        const std::string declarations = std::string("//VTK::Cropping::Dec\n")
            + "uniform sampler1D mvvcvtk_cropTable;\nuniform int mvvcvtk_cropNodeCount;\n"
            + kPredicateCode;
        const std::string implementation =
            "//VTK::Cropping::Impl\n"
            "vec3 mvvcvtk_cropPointMC = (in_textureDatasetMatrix[0] * vec4(g_dataPos, 1.0)).xyz;\n"
            "if (!mvvcvtkCropKept(mvvcvtk_cropPointMC)) { g_skip = true; }\n";
        shaderProperty->AddFragmentShaderReplacement(
            "//VTK::Cropping::Dec", true, declarations, false);
        shaderProperty->AddFragmentShaderReplacement(
            "//VTK::Cropping::Impl", true, implementation, false);
    }
    else {
        const bool hasMatrix = m_targetKind == RenderTargetKind::Slice;
        std::string vertexDec = "//VTK::PositionVC::Dec\nout vec3 mvvcvtk_cropPointMC;\n";
        if (hasMatrix) {
            vertexDec += "uniform mat4 mvvcvtk_localToInput;\n";
        }
        const std::string vertexImpl = hasMatrix
            ? "//VTK::PositionVC::Impl\nmvvcvtk_cropPointMC = (mvvcvtk_localToInput * vertexMC).xyz;\n"
            : "//VTK::PositionVC::Impl\nmvvcvtk_cropPointMC = vertexMC.xyz;\n";
        const std::string fragmentDec = GetPolyDec("//VTK::PositionVC::Dec", false);
        const std::string fragmentImpl =
            "//VTK::Color::Impl\n"
            "if (!mvvcvtkCropKept(mvvcvtk_cropPointMC)) { discard; }\n";
        shaderProperty->AddVertexShaderReplacement(
            "//VTK::PositionVC::Dec", true, vertexDec, false);
        shaderProperty->AddVertexShaderReplacement(
            "//VTK::PositionVC::Impl", true, vertexImpl, false);
        shaderProperty->AddFragmentShaderReplacement(
            "//VTK::PositionVC::Dec", true, fragmentDec, false);
        shaderProperty->AddFragmentShaderReplacement(
            "//VTK::Color::Impl", true, fragmentImpl, false);
    }

    m_observer = vtkSmartPointer<vtkCallbackCommand>::New();
    m_observer->SetClientData(this);
    m_observer->SetCallback(&Impl::OnShader);
    m_observerTag = mapper->AddObserver(vtkCommand::UpdateShaderEvent, m_observer);
    m_mapper = mapper;
    return m_observerTag != 0;
}

bool CropShaderController::Impl::SetCropParams(CropShaderPayload payload)
{
    if (payload.revision == 0
        || !payload.sourceStamp.identity
        || payload.sourceStamp.version == 0
        || !payload.predicateTable
        || payload.nodeCount > payload.predicateTable->operationCount
        || payload.predicateTable->rgbaValues.size()
            != payload.predicateTable->operationCount
                * CropAlgorithm::GetTexelCount() * 4
        || m_commitRevision != 0
        || payload.revision <= m_active.payload.revision
        || payload.revision <= m_staged.payload.revision) {
        return false;
    }

    m_staged = {};
    m_staged.payload = std::move(payload);
    m_state.status = RenderEffectStatus::Staged;
    m_state.failureReason = RenderEffectFailure::None;
    m_state.stagedRevision = m_staged.payload.revision;
    m_state.message.clear();
    m_stageRenderCount = 0;
    return true;
}

bool CropShaderController::Impl::BuildTexture(vtkOpenGLRenderWindow* context)
{
    if (m_staged.payload.revision == 0 || m_staged.texture) {
        return true;
    }
    if (!context
        || !vtkTextureObject::IsSupported(context, true, false, false)) {
        m_state.status = RenderEffectStatus::Failed;
        m_state.failureReason = RenderEffectFailure::ContextLost;
        m_state.message = "The render context cannot create float crop textures.";
        return false;
    }

    if (m_staged.payload.predicateTable == m_active.payload.predicateTable
        && m_active.texture) {
        m_staged.texture = m_active.texture;
        m_state.status = RenderEffectStatus::Ready;
        return true;
    }

    const auto& values = m_staged.payload.predicateTable->rgbaValues;
    const std::size_t width = values.size() / 4;
    if (width == 0
        || width > static_cast<std::size_t>(vtkTextureObject::GetMaximumTextureSize(context))) {
        m_state.status = RenderEffectStatus::Failed;
        m_state.failureReason = RenderEffectFailure::TextureFailed;
        m_state.message = "The crop table exceeds the context texture-width limit.";
        return false;
    }

    auto texture = vtkSmartPointer<vtkTextureObject>::New();
    texture->SetContext(context);
    texture->SetInternalFormat(GL_RGBA32F);
    texture->SetFormat(GL_RGBA);
    texture->SetDataType(GL_FLOAT);
    texture->SetWrapS(vtkTextureObject::ClampToEdge);
    texture->SetMinificationFilter(vtkTextureObject::Nearest);
    texture->SetMagnificationFilter(vtkTextureObject::Nearest);
    // VTK 9.4 的上传 API 错误地把只读源声明为 void*；实现只读取该缓冲区。
    auto* uploadValues = const_cast<float*>(values.data());
    if (!texture->Create1DFromRaw(
            static_cast<unsigned int>(width), 4, VTK_FLOAT, uploadValues)) {
        m_state.status = RenderEffectStatus::Failed;
        m_state.failureReason = RenderEffectFailure::TextureFailed;
        m_state.message = "The RGBA32F crop table upload failed.";
        return false;
    }
    m_staged.texture = std::move(texture);
    return true;
}

void CropShaderController::Impl::ClearDeferred(vtkOpenGLRenderWindow* context)
{
    for (auto& texture : m_deferred) {
        if (texture) {
            texture->ReleaseGraphicsResources(context);
        }
    }
    m_deferred.clear();
}

bool CropShaderController::Impl::StartRender(vtkRenderer* renderer)
{
    auto* context = renderer
        ? vtkOpenGLRenderWindow::SafeDownCast(renderer->GetRenderWindow())
        : nullptr;
    if (!context) {
        if (m_staged.payload.revision != 0
            || m_active.payload.revision != 0) {
            m_state.status = RenderEffectStatus::Failed;
            m_state.failureReason = RenderEffectFailure::ContextLost;
            m_state.message = "No OpenGL render context is bound.";
        }
        return false;
    }
    if (m_context && m_context.GetPointer() != context) {
        m_state.status = RenderEffectStatus::Failed;
        m_state.failureReason = RenderEffectFailure::ContextLost;
        m_state.message = "A crop shader resource cannot migrate between OpenGL contexts.";
        return false;
    }
    context->MakeCurrent();
    m_hasProgramSync = false;
    m_context = context;
    ClearDeferred(context);
    (void)BuildTexture(context);
    // node 0 仍绑定 active table，确保既有 shader program 每帧都收到 nodeCount=0。
    // 若在这里把纹理解绑，部分 Volume mapper 不再产生 UpdateShaderEvent，旧的正数
    // uniform 会残留并继续丢弃全部采样点。
    m_boundTexture = m_active.texture
        ? m_active.texture
        : m_staged.texture;
    if (m_boundTexture) {
        m_boundTexture->Activate();
        m_isActive = true;
    }
    // UpdateShaderEvent 并非每帧触发；历史归零只改变 nodeCount，
    // 且会复用既有 table/program，因此必须主动刷新缓存 program 的 uniform。
    if (auto* program = m_program.GetPointer()) {
        auto* shaderCache = context->GetShaderCache();
        SetProgram(shaderCache
                ? shaderCache->ReadyShaderProgram(program)
                : nullptr);
    }
    return true;
}

bool CropShaderController::Impl::StopRender()
{
    if (m_isActive && m_boundTexture) {
        m_boundTexture->Deactivate();
    }
    m_boundTexture = nullptr;
    m_isActive = false;
    if (m_staged.payload.revision != 0
        && m_staged.texture
        && m_state.status == RenderEffectStatus::Staged
        && !m_hasProgramSync
        && ++m_stageRenderCount >= 2) {
        m_state.status = RenderEffectStatus::Failed;
        m_state.failureReason = RenderEffectFailure::CompileFailed;
        m_state.message = "The crop shader did not produce an UpdateShaderEvent.";
    }
    return true;
}

void CropShaderController::Impl::OnShader(
    vtkObject*, unsigned long, void* clientData, void* callData)
{
    auto* self = static_cast<Impl*>(clientData);
    self->SetProgram(static_cast<vtkShaderProgram*>(callData));
}

void CropShaderController::Impl::SetProgram(vtkShaderProgram* program)
{
    if (!program) {
        return;
    }
    m_program = program;
    m_hasProgramSync = true;
    if (m_staged.payload.revision != 0
        && m_staged.texture
        && m_state.status == RenderEffectStatus::Staged) {
        m_state.status = RenderEffectStatus::Ready;
    }
    const int nodeCount = m_isActive
        ? static_cast<int>(m_active.payload.nodeCount)
        : 0;
    program->SetUniformi("mvvcvtk_cropNodeCount", nodeCount);
    if (m_isActive && m_boundTexture) {
        program->SetUniformi("mvvcvtk_cropTable", m_boundTexture->GetTextureUnit());
    }
    if (m_targetKind == RenderTargetKind::Slice) {
        float localToInput[16] = {};
        // 矩阵按 VTK row-major 保存；glUniformMatrix4fv 的
        // transpose 参数固定为 false，因此上传前必须显式转成 column-major。
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                localToInput[column * 4 + row] = static_cast<float>(
                    m_localToInput[row * 4 + column]);
            }
        }
        program->SetUniformMatrix4x4("mvvcvtk_localToInput", localToInput);
    }
}

bool CropShaderController::Impl::SetCropCommit(const std::uint64_t revision)
{
    if (revision == 0
        || revision != m_staged.payload.revision
        || m_state.status != RenderEffectStatus::Ready) {
        return false;
    }
    m_previous = std::move(m_active);
    m_active = std::move(m_staged);
    m_staged = {};
    m_commitRevision = revision;
    m_state.status = RenderEffectStatus::Committed;
    m_state.activeRevision = revision;
    m_state.stagedRevision = 0;
    return true;
}

bool CropShaderController::Impl::GetCropCommitReady(
    const std::uint64_t revision) const
{
    return revision != 0 && revision == m_commitRevision;
}

bool CropShaderController::Impl::SetCropComplete(
    const std::uint64_t revision)
{
    if (!GetCropCommitReady(revision)) {
        return false;
    }
    if (m_previous.texture
        && m_previous.texture != m_active.texture) {
        m_deferred.push_back(m_previous.texture);
    }
    m_previous = {};
    m_commitRevision = 0;
    return true;
}

bool CropShaderController::Impl::ClearCropCommit(
    const std::uint64_t revision)
{
    if (revision == 0 || revision != m_commitRevision) {
        return false;
    }
    if (m_active.texture
        && m_active.texture != m_previous.texture) {
        m_deferred.push_back(m_active.texture);
    }
    m_active = std::move(m_previous);
    m_previous = {};
    m_commitRevision = 0;
    m_state.status = m_active.payload.revision == 0
        ? RenderEffectStatus::Idle
        : RenderEffectStatus::Committed;
    m_state.failureReason = RenderEffectFailure::None;
    m_state.activeRevision = m_active.payload.revision;
    m_state.stagedRevision = 0;
    m_state.message.clear();
    return true;
}

bool CropShaderController::Impl::ClearCropStage(const std::uint64_t revision)
{
    if (revision != 0 && revision == m_staged.payload.revision) {
        if (m_staged.texture && m_staged.texture != m_active.texture) {
            m_deferred.push_back(m_staged.texture);
        }
        m_staged = {};
        m_state.stagedRevision = 0;
        m_state.status = m_active.payload.revision == 0
            ? RenderEffectStatus::Idle
            : RenderEffectStatus::Committed;
        m_state.failureReason = RenderEffectFailure::None;
        m_state.message.clear();
    }
    return true;
}

bool CropShaderController::Impl::ClearCropParams()
{
    if (m_previous.texture
        && m_previous.texture != m_active.texture
        && m_previous.texture != m_staged.texture) {
        m_deferred.push_back(m_previous.texture);
    }
    if (m_active.texture) {
        m_deferred.push_back(m_active.texture);
    }
    if (m_staged.texture && m_staged.texture != m_active.texture) {
        m_deferred.push_back(m_staged.texture);
    }
    m_previous = {};
    m_active = {};
    m_staged = {};
    m_commitRevision = 0;
    m_state = {};
    return true;
}

bool CropShaderController::Impl::SetLocalToInput(
    const std::array<double, 16>& localToInput)
{
    m_localToInput = localToInput;
    return true;
}

CropShaderController::CropShaderController(const RenderTargetKind targetKind)
    : m_impl(std::make_unique<Impl>(targetKind))
{
}

CropShaderController::~CropShaderController() = default;

bool CropShaderController::SetShaderTarget(vtkObject* mapper, vtkShaderProperty* shaderProperty)
{
    return m_impl->SetShaderTarget(mapper, shaderProperty);
}

bool CropShaderController::SetCropParams(CropShaderPayload payload)
{
    return m_impl->SetCropParams(std::move(payload));
}

RenderEffectState CropShaderController::GetState() const { return m_impl->GetState(); }
bool CropShaderController::SetCropCommit(const std::uint64_t revision) { return m_impl->SetCropCommit(revision); }
bool CropShaderController::GetCropCommitReady(const std::uint64_t revision) const { return m_impl->GetCropCommitReady(revision); }
bool CropShaderController::SetCropComplete(const std::uint64_t revision) { return m_impl->SetCropComplete(revision); }
bool CropShaderController::ClearCropCommit(const std::uint64_t revision) { return m_impl->ClearCropCommit(revision); }
bool CropShaderController::ClearCropStage(const std::uint64_t revision) { return m_impl->ClearCropStage(revision); }
bool CropShaderController::ClearCropParams() { return m_impl->ClearCropParams(); }
bool CropShaderController::SetLocalToInput(const std::array<double, 16>& matrix) { return m_impl->SetLocalToInput(matrix); }
bool CropShaderController::StartRender(vtkRenderer* renderer) { return m_impl->StartRender(renderer); }
bool CropShaderController::StopRender() { return m_impl->StopRender(); }

namespace {
class CropEffectBinding final : public RenderEffectBinding {
public:
    CropEffectBinding(
        const RenderEffectTarget& target,
        const RenderBindingUse bindingUse)
        : m_controller(target.targetKind)
        , m_inputStamp(target.inputStamp)
        , m_bindingUse(bindingUse)
    {
        m_isTargetReady = m_controller.SetShaderTarget(
            target.mapper, target.shaderProperty);
        m_isTargetReady = m_isTargetReady
            && m_controller.SetLocalToInput(
                target.localToInput);
    }

    bool GetTargetReady() const { return m_isTargetReady; }

    bool SetCropParams(CropShaderPayload payload)
    {
        if (!m_isTargetReady
            || payload.sourceStamp != m_inputStamp) {
            return false;
        }
        m_replayRevision = 0;
        return m_controller.SetCropParams(std::move(payload));
    }

    bool SetCommittedReplay(CropShaderPayload payload)
    {
        const auto revision = payload.revision;
        if (!SetCropParams(std::move(payload))) {
            return false;
        }
        m_replayRevision = revision;
        return true;
    }

    RenderInputStamp GetInputStamp() const override
    {
        return m_inputStamp;
    }

    RenderBindingUse GetBindingUse() const override
    {
        return m_bindingUse;
    }

    RenderEffectState GetEffectState() const override
    {
        return m_controller.GetState();
    }

    bool SetBindingUse(const RenderBindingUse bindingUse) override
    {
        if (m_bindingUse == bindingUse) {
            return true;
        }
        if (m_controller.GetState().status == RenderEffectStatus::Staged) {
            return false;
        }
        m_bindingUse = bindingUse;
        return true;
    }

    bool SetEffectCommit(const std::uint64_t revision) override
    {
        if (!SetCropCommit(revision)) {
            return false;
        }
        if (SetCropComplete(revision)) {
            return true;
        }
        (void)ClearCropCommit(revision);
        return false;
    }

    bool SetCropCommit(const std::uint64_t revision)
    {
        m_replayRevision = 0;
        return m_controller.SetCropCommit(revision);
    }

    bool GetCropCommitReady(const std::uint64_t revision) const
    {
        return m_controller.GetCropCommitReady(revision);
    }

    bool SetCropComplete(const std::uint64_t revision)
    {
        return m_controller.SetCropComplete(revision);
    }

    bool ClearCropCommit(const std::uint64_t revision)
    {
        return m_controller.ClearCropCommit(revision);
    }

    bool ClearEffectStage(const std::uint64_t revision) override
    {
        if (revision == m_replayRevision) {
            m_replayRevision = 0;
        }
        return m_controller.ClearCropStage(revision);
    }

    bool ResetEffect() override
    {
        m_replayRevision = 0;
        return m_controller.ClearCropParams();
    }

    bool SetLocalToInput(
        const std::array<double, 16>& localToInput) override
    {
        return m_controller.SetLocalToInput(localToInput);
    }

    bool SetRenderInput(const RenderInputStamp inputStamp) override
    {
        if (inputStamp == m_inputStamp) {
            return true;
        }
        if (!ResetEffect()) {
            return false;
        }
        m_inputStamp = inputStamp;
        return true;
    }

    bool OnRenderStart(vtkRenderer* renderer) override
    {
        return m_controller.StartRender(renderer);
    }

    bool OnRenderStop() override
    {
        if (!m_controller.StopRender()) {
            return false;
        }
        const auto state = m_controller.GetState();
        if (m_replayRevision != 0
            && state.status == RenderEffectStatus::Ready
            && state.stagedRevision == m_replayRevision) {
            const auto revision = m_replayRevision;
            m_replayRevision = 0;
            return SetEffectCommit(revision);
        }
        return true;
    }

private:
    CropShaderController m_controller;
    RenderInputStamp m_inputStamp;
    RenderBindingUse m_bindingUse = RenderBindingUse::Current;
    std::uint64_t m_replayRevision = 0;
    bool m_isTargetReady = false;
};
}

class CropShaderEffect::Impl final {
public:
    bool SetCropParams(CropShaderPayload payload);
    RenderEffectState GetState() const;
    bool StartCropCommit(std::uint64_t revision);
    bool GetCropCommitReady(std::uint64_t revision) const;
    bool SetCropCommit(std::uint64_t revision);
    bool SetCropComplete(std::uint64_t revision);
    bool ClearCropCommit(std::uint64_t revision);
    bool ClearCropStage(std::uint64_t revision);
    bool ClearCropParams();
    std::shared_ptr<RenderEffectBinding> BuildEffectBinding(
        const RenderEffectTarget& target,
        RenderBindingUse bindingUse);

private:
    std::vector<std::shared_ptr<CropEffectBinding>> GetBindings() const;
    std::vector<std::shared_ptr<CropEffectBinding>>
        GetCurrentBindings() const;
    std::vector<std::shared_ptr<CropEffectBinding>>
        GetStagedBindings() const;

    mutable std::vector<std::weak_ptr<CropEffectBinding>> m_bindings;
    std::vector<std::weak_ptr<CropEffectBinding>> m_stagedBindings;
    std::vector<std::weak_ptr<CropEffectBinding>> m_commitBindings;
    CropShaderPayload m_previous;
    CropShaderPayload m_active;
    CropShaderPayload m_staged;
    RenderEffectState m_state;
    std::uint64_t m_commitRevision = 0;
};

std::vector<std::shared_ptr<CropEffectBinding>>
CropShaderEffect::Impl::GetBindings() const
{
    std::vector<std::shared_ptr<CropEffectBinding>> bindings;
    auto output = m_bindings.begin();
    for (auto input = m_bindings.begin();
        input != m_bindings.end(); ++input) {
        auto binding = input->lock();
        if (!binding) {
            continue;
        }
        *output++ = *input;
        bindings.push_back(std::move(binding));
    }
    m_bindings.erase(output, m_bindings.end());
    return bindings;
}

std::vector<std::shared_ptr<CropEffectBinding>>
CropShaderEffect::Impl::GetCurrentBindings() const
{
    auto bindings = GetBindings();
    bindings.erase(
        std::remove_if(
            bindings.begin(),
            bindings.end(),
            [](const auto& binding) {
                return binding->GetBindingUse()
                    != RenderBindingUse::Current;
            }),
        bindings.end());
    return bindings;
}

std::vector<std::shared_ptr<CropEffectBinding>>
CropShaderEffect::Impl::GetStagedBindings() const
{
    std::vector<std::shared_ptr<CropEffectBinding>> bindings;
    bindings.reserve(m_stagedBindings.size());
    for (const auto& weakBinding : m_stagedBindings) {
        if (auto binding = weakBinding.lock()) {
            bindings.push_back(std::move(binding));
        }
    }
    return bindings;
}

bool CropShaderEffect::Impl::SetCropParams(CropShaderPayload payload)
{
    if (payload.revision == 0
        || !payload.sourceStamp.identity
        || payload.sourceStamp.version == 0
        || !payload.predicateTable
        || m_commitRevision != 0
        || m_staged.revision != 0
        || payload.revision <= m_active.revision) {
        return false;
    }

    auto currentBindings = GetCurrentBindings();
    if (currentBindings.empty()) {
        return false;
    }

    std::vector<std::shared_ptr<CropEffectBinding>> stagedBindings;
    for (const auto& binding : currentBindings) {
        if (!binding->SetCropParams(payload)) {
            for (const auto& stagedBinding : stagedBindings) {
                (void)stagedBinding->ClearEffectStage(payload.revision);
            }
            return false;
        }
        stagedBindings.push_back(binding);
    }

    m_staged = std::move(payload);
    m_stagedBindings.assign(
        stagedBindings.begin(), stagedBindings.end());
    m_state.status = RenderEffectStatus::Staged;
    m_state.failureReason = RenderEffectFailure::None;
    m_state.stagedRevision = m_staged.revision;
    m_state.message.clear();
    return true;
}

RenderEffectState CropShaderEffect::Impl::GetState() const
{
    if (m_staged.revision == 0) {
        return m_state;
    }
    auto state = m_state;
    const auto currentBindings = GetCurrentBindings();
    const auto stagedBindings = GetStagedBindings();
    const bool isComplete =
        stagedBindings.size() == m_stagedBindings.size()
        && currentBindings.size() == stagedBindings.size()
        && std::all_of(
            stagedBindings.begin(),
            stagedBindings.end(),
            [&currentBindings](const auto& binding) {
                return std::find(
                    currentBindings.begin(),
                    currentBindings.end(),
                    binding) != currentBindings.end();
            });
    if (!isComplete) {
        state.status = RenderEffectStatus::Failed;
        state.failureReason = RenderEffectFailure::ContextLost;
        state.message =
            "A staged crop target was detached or replaced before commit.";
        return state;
    }

    bool isReady = true;
    for (const auto& binding : stagedBindings) {
        const auto bindingState = binding->GetEffectState();
        if (bindingState.status == RenderEffectStatus::Failed) {
            return bindingState;
        }
        if (bindingState.status != RenderEffectStatus::Ready
            || bindingState.stagedRevision != m_staged.revision) {
            isReady = false;
        }
    }
    if (isReady) {
        state.status = RenderEffectStatus::Ready;
    }
    return state;
}

bool CropShaderEffect::Impl::StartCropCommit(
    const std::uint64_t revision)
{
    if (revision == 0
        || m_commitRevision != 0
        || revision != m_staged.revision) {
        return false;
    }
    const auto currentBindings = GetCurrentBindings();
    const auto stagedBindings = GetStagedBindings();
    if (stagedBindings.size() != m_stagedBindings.size()
        || currentBindings.size() != stagedBindings.size()
        || !std::all_of(
            stagedBindings.begin(),
            stagedBindings.end(),
            [&currentBindings](const auto& binding) {
                return std::find(
                    currentBindings.begin(),
                    currentBindings.end(),
                    binding) != currentBindings.end();
            })) {
        return false;
    }
    for (const auto& binding : stagedBindings) {
        const auto state = binding->GetEffectState();
        if (state.status != RenderEffectStatus::Ready
            || state.stagedRevision != revision) {
            return false;
        }
    }
    std::vector<std::shared_ptr<CropEffectBinding>>
        committedBindings;
    committedBindings.reserve(stagedBindings.size());
    for (const auto& binding : stagedBindings) {
        if (!binding->SetCropCommit(revision)) {
            for (auto committed = committedBindings.rbegin();
                committed != committedBindings.rend();
                ++committed) {
                (void)(*committed)->ClearCropCommit(revision);
            }
            for (const auto& stagedBinding : stagedBindings) {
                (void)stagedBinding->ClearEffectStage(revision);
            }
            m_staged = {};
            m_stagedBindings.clear();
            m_state.status = RenderEffectStatus::Failed;
            m_state.failureReason =
                RenderEffectFailure::ContextLost;
            m_state.stagedRevision = 0;
            m_state.message =
                "A crop target failed during the atomic commit.";
            return false;
        }
        committedBindings.push_back(binding);
    }
    // Effect 根只进入可逆 commit；Bridge 对全部 Effect 成功后才 complete。
    // 这样后续目标失败时，已切换的 binding 仍可恢复上一版 active。
    m_previous = std::move(m_active);
    m_active = std::move(m_staged);
    m_staged = {};
    m_stagedBindings.clear();
    m_commitBindings.assign(
        committedBindings.begin(),
        committedBindings.end());
    m_commitRevision = revision;
    m_state.status = RenderEffectStatus::Committed;
    m_state.failureReason = RenderEffectFailure::None;
    m_state.activeRevision = revision;
    m_state.stagedRevision = 0;
    m_state.message.clear();
    return true;
}

bool CropShaderEffect::Impl::SetCropCommit(
    const std::uint64_t revision)
{
    if (!StartCropCommit(revision)) {
        return false;
    }
    if (SetCropComplete(revision)) {
        return true;
    }
    (void)ClearCropCommit(revision);
    return false;
}

bool CropShaderEffect::Impl::GetCropCommitReady(
    const std::uint64_t revision) const
{
    if (revision == 0 || revision != m_commitRevision) {
        return false;
    }
    const auto currentBindings = GetCurrentBindings();
    if (currentBindings.size() != m_commitBindings.size()) {
        return false;
    }
    for (const auto& weakBinding : m_commitBindings) {
        const auto binding = weakBinding.lock();
        if (!binding
            || std::find(
                currentBindings.begin(),
                currentBindings.end(),
                binding) == currentBindings.end()
            || !binding->GetCropCommitReady(revision)) {
            return false;
        }
    }
    return true;
}

bool CropShaderEffect::Impl::SetCropComplete(
    const std::uint64_t revision)
{
    // 先验证所有 binding，再释放任何 previous 资源。Bridge 会对所有 Effect
    // 做同样的全局预检，因此进入完成循环后不再存在可预期的部分失败点。
    if (!GetCropCommitReady(revision)) {
        return false;
    }
    std::vector<std::shared_ptr<CropEffectBinding>> bindings;
    bindings.reserve(m_commitBindings.size());
    for (const auto& weakBinding : m_commitBindings) {
        auto binding = weakBinding.lock();
        if (!binding) {
            return false;
        }
        bindings.push_back(std::move(binding));
    }
    for (const auto& binding : bindings) {
        if (!binding->SetCropComplete(revision)) {
            return false;
        }
    }
    m_previous = {};
    m_commitBindings.clear();
    m_commitRevision = 0;
    return true;
}

bool CropShaderEffect::Impl::ClearCropCommit(
    const std::uint64_t revision)
{
    if (revision == 0 || revision != m_commitRevision) {
        return false;
    }
    std::vector<std::shared_ptr<CropEffectBinding>> bindings;
    bindings.reserve(m_commitBindings.size());
    for (const auto& weakBinding : m_commitBindings) {
        auto binding = weakBinding.lock();
        if (!binding) {
            return false;
        }
        bindings.push_back(std::move(binding));
    }
    for (auto binding = bindings.rbegin();
        binding != bindings.rend();
        ++binding) {
        if (!(*binding)->ClearCropCommit(revision)) {
            return false;
        }
    }
    m_active = std::move(m_previous);
    m_previous = {};
    m_commitBindings.clear();
    m_commitRevision = 0;
    m_state.status = m_active.revision == 0
        ? RenderEffectStatus::Idle
        : RenderEffectStatus::Committed;
    m_state.failureReason = RenderEffectFailure::None;
    m_state.activeRevision = m_active.revision;
    m_state.stagedRevision = 0;
    m_state.message.clear();
    return true;
}

bool CropShaderEffect::Impl::ClearCropStage(
    const std::uint64_t revision)
{
    bool isCleared = true;
    for (const auto& binding : GetBindings()) {
        isCleared = binding->ClearEffectStage(revision) && isCleared;
    }
    if (revision == m_staged.revision) {
        m_staged = {};
        m_stagedBindings.clear();
        m_state.status = m_active.revision == 0
            ? RenderEffectStatus::Idle
            : RenderEffectStatus::Committed;
        m_state.failureReason = RenderEffectFailure::None;
        m_state.stagedRevision = 0;
        m_state.message.clear();
    }
    return isCleared;
}

bool CropShaderEffect::Impl::ClearCropParams()
{
    bool isCleared = true;
    for (const auto& binding : GetBindings()) {
        isCleared = binding->ResetEffect() && isCleared;
    }
    m_active = {};
    m_staged = {};
    m_previous = {};
    m_stagedBindings.clear();
    m_commitBindings.clear();
    m_commitRevision = 0;
    m_state = {};
    return isCleared;
}

std::shared_ptr<RenderEffectBinding>
CropShaderEffect::Impl::BuildEffectBinding(
    const RenderEffectTarget& target,
    const RenderBindingUse bindingUse)
{
    if (target.targetKind == RenderTargetKind::Unknown
        || !target.mapper
        || !target.shaderProperty
        || !target.inputStamp.identity
        || target.inputStamp.version == 0) {
        return {};
    }
    auto binding = std::make_shared<CropEffectBinding>(
        target, bindingUse);
    if (!binding->GetTargetReady()) {
        return {};
    }
    if (m_active.revision != 0
        && m_active.sourceStamp == target.inputStamp
        && !binding->SetCommittedReplay(m_active)) {
        return {};
    }
    m_bindings.push_back(binding);
    return binding;
}

CropShaderEffect::CropShaderEffect()
    : m_impl(std::make_unique<Impl>())
{
}

CropShaderEffect::~CropShaderEffect() = default;

bool CropShaderEffect::SetCropParams(CropShaderPayload payload)
{
    return m_impl->SetCropParams(std::move(payload));
}

RenderEffectState CropShaderEffect::GetState() const
{
    return m_impl->GetState();
}

bool CropShaderEffect::StartCropCommit(
    const std::uint64_t revision)
{
    return m_impl->StartCropCommit(revision);
}

bool CropShaderEffect::GetCropCommitReady(
    const std::uint64_t revision) const
{
    return m_impl->GetCropCommitReady(revision);
}

bool CropShaderEffect::SetCropCommit(const std::uint64_t revision)
{
    return m_impl->SetCropCommit(revision);
}

bool CropShaderEffect::SetCropComplete(
    const std::uint64_t revision)
{
    return m_impl->SetCropComplete(revision);
}

bool CropShaderEffect::ClearCropCommit(
    const std::uint64_t revision)
{
    return m_impl->ClearCropCommit(revision);
}

bool CropShaderEffect::ClearCropStage(const std::uint64_t revision)
{
    return m_impl->ClearCropStage(revision);
}

bool CropShaderEffect::ClearCropParams()
{
    return m_impl->ClearCropParams();
}

std::shared_ptr<RenderEffectBinding>
CropShaderEffect::BuildEffectBinding(
    const RenderEffectTarget& target,
    const RenderBindingUse bindingUse)
{
    return m_impl->BuildEffectBinding(target, bindingUse);
}
