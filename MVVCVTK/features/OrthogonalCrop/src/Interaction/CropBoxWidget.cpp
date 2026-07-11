// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Interaction/CropBoxWidget.cpp
// 分类: Service / Widget Controller Implementation
// 说明: 封装 vtkBoxWidget2 的构造、启停、observer 绑定与 bounds 回调翻译。
// =====================================================================
// 这个控制器刻意保持“薄”：
// - 下层只接触 VTK widget 细节
// - 上层只拿到 bounds 与 phase
// - 中间不掺入任何 preview 或算法逻辑，便于保持交互链路职责清晰

#include "Interaction/CropBoxWidget.h"

#include <vtkBoxRepresentation.h>
#include <vtkBoxWidget2.h>
#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <memory>
#include <utility>

static constexpr double kBoxOutlineNormalRed = 0.74;
static constexpr double kBoxOutlineNormalGreen = 0.80;
static constexpr double kBoxOutlineNormalBlue = 0.86;
static constexpr double kBoxOutlineSelectedRed = 1.0;
static constexpr double kBoxOutlineSelectedGreen = 0.68;
static constexpr double kBoxOutlineSelectedBlue = 0.16;
static constexpr double kBoxLineWidth = 2.0;
static constexpr double kBoxHotWidth = 2.8;
static constexpr double kBoxFaceOpacity = 0.0;

class CropBoxWidget::Impl final {
public:
    using BoundsCallback = CropBoxWidget::BoundsCallback;

    Impl();
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void SetInteractor(vtkRenderWindowInteractor* interactor);
    void SetReferenceWorldBounds(const std::array<double, 6>& worldBounds);
    void SetWidgetWorldBounds(const std::array<double, 6>& worldBounds);
    const std::array<double, 6>& GetCurrentWorldBounds() const;
    bool GetCurrentWorldBox(
        CropVectorDouble3Array& baseCenter,
        CropVectorDouble3Array& baseSize,
        CropMatrixDouble16Array& baseToNow) const;
    void SetBoundsCallback(BoundsCallback callback);
    bool SetEnabled(bool isEnabled);
    bool GetEnabled() const;

private:
    static void SendWidgetEvent(
        vtkObject* caller,
        unsigned long eventId,
        void* clientData,
        void* callData);
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);
    static CropInteractionPhase GetEventPhase(unsigned long eventId);

    void AttachObservers();
    void OnWidgetEvent(unsigned long eventId);
    void SetVisualState(CropInteractionPhase phase);

    // 非拥有事件源；SetInteractor 注入，启用期间必须有效，Impl 不负责删除。
    vtkRenderWindowInteractor* m_interactor = nullptr;
    // Impl 持有 widget/representation 的 VTK 引用；widget 同时引用 representation。
    vtkSmartPointer<vtkBoxWidget2> m_widget;
    vtkSmartPointer<vtkBoxRepresentation> m_representation;
    // Impl 持有 VTK 回调命令；clientData 指回 this，析构时先清空以切断悬空回调入口。
    vtkSmartPointer<vtkCallbackCommand> m_callbackCommand;
    // 上层回调值副本；VTK 事件线程同步消费，SetBoundsCallback 替换。
    BoundsCallback m_boundsCallback;
    // 最近一次成功 On/Off 后的逻辑状态；启用前置校验失败时不改写。
    bool m_isEnabled = false;
    // observer 只懒绑定一次；tag 由 AddObserver 生产，Impl 析构逐项 RemoveObserver。
    bool m_hasObservers = false;
    std::array<unsigned long, 3> m_observerTags = { 0, 0, 0 };
    // representation 当前 world AABB 派生缓存；交互事件更新，不能表达有向盒旋转姿态。
    std::array<double, 6> m_currentWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 最近一次 PlaceWidget 的 world AABB 基准快照；与当前角点一起反解 base-to-current affine。
    std::array<double, 6> m_baseBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 调用方提供的 world AABB 回退值；当前 bounds 无效或首次启用时消费。
    std::array<double, 6> m_referenceWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
};

CropBoxWidget::Impl::Impl()
{
    // 初始化 widget 和 representation；
    // 普通态使用中性色，是因为裁切盒只是空间手柄；只有被 VTK 选中/拖拽时才用高亮色提示交互焦点。
    m_widget = vtkSmartPointer<vtkBoxWidget2>::New();
    m_representation = vtkSmartPointer<vtkBoxRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);

    // BOX 裁切只让线框承载交互反馈；
    // VTK 默认 selected face 会显示半透明黄面，容易被误读成裁切结果或模型被染色，所以这里显式保持面透明。
    m_representation->GetFaceProperty()->SetOpacity(kBoxFaceOpacity);
    m_representation->GetSelectedFaceProperty()->SetOpacity(kBoxFaceOpacity);

    m_representation->GetOutlineProperty()->SetColor(
        kBoxOutlineNormalRed,
        kBoxOutlineNormalGreen,
        kBoxOutlineNormalBlue);
    m_representation->GetOutlineProperty()->SetLineWidth(kBoxLineWidth);
    m_representation->GetSelectedOutlineProperty()->SetColor(
        kBoxOutlineSelectedRed,
        kBoxOutlineSelectedGreen,
        kBoxOutlineSelectedBlue);
    m_representation->GetSelectedOutlineProperty()->SetLineWidth(kBoxHotWidth);
    m_widget->SetRepresentation(m_representation);

    // VTK callback 只做事件投递；交互相位和 preview 触发策略统一留给 Impl 与 bridge。
    m_callbackCommand = vtkSmartPointer<vtkCallbackCommand>::New();
    m_callbackCommand->SetClientData(this);
    m_callbackCommand->SetCallback(&CropBoxWidget::Impl::SendWidgetEvent);
}

CropBoxWidget::Impl::~Impl()
{
    if (m_widget && m_hasObservers) {
        for (const auto observerTag : m_observerTags) {
            if (observerTag != 0) {
                m_widget->RemoveObserver(observerTag);
            }
        }
    }

    if (m_callbackCommand) {
        m_callbackCommand->SetClientData(nullptr);
    }
}

void CropBoxWidget::Impl::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    // widget 与 interactor 的绑定始终在这里完成，避免上层直接碰 vtkBoxWidget2。
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void CropBoxWidget::Impl::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    // referenceWorldBounds 是 Enable 时的回退基准，同时也是第一次 PlaceWidget 的边界来源。
    m_referenceWorldBounds = worldBounds;
    if (!GetBoundsAreValid(m_currentWorldBounds)) {
        m_currentWorldBounds = worldBounds;
    }
    m_baseBounds = m_currentWorldBounds;
    m_representation->PlaceWidget(m_currentWorldBounds.data());
}

void CropBoxWidget::Impl::SetWidgetWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_currentWorldBounds = worldBounds;
    m_baseBounds = worldBounds;
    m_representation->PlaceWidget(m_currentWorldBounds.data());
}

const std::array<double, 6>& CropBoxWidget::Impl::GetCurrentWorldBounds() const
{
    return m_currentWorldBounds;
}

bool CropBoxWidget::Impl::GetCurrentWorldBox(
    CropVectorDouble3Array& baseCenter,
    CropVectorDouble3Array& baseSize,
    CropMatrixDouble16Array& baseToNow) const
{
    if (!m_representation || !GetBoundsAreValid(m_baseBounds)) {
        return false;
    }

    // 先把最近一次 PlaceWidget 的 world AABB 拆成中心和尺寸；
    // 这两个变量描述“未交互前的基准盒”，后续用它把标准盒放回同一个 world 体积。
    baseCenter = {
        (m_baseBounds[0] + m_baseBounds[1]) * 0.5,
        (m_baseBounds[2] + m_baseBounds[3]) * 0.5,
        (m_baseBounds[4] + m_baseBounds[5]) * 0.5
    };
    baseSize = {
        m_baseBounds[1] - m_baseBounds[0],
        m_baseBounds[3] - m_baseBounds[2],
        m_baseBounds[5] - m_baseBounds[4]
    };

    // 再从当前 widget polydata 读取交互后的 world 角点；
    // polydata 保留旋转姿态，GetBounds 只给外接 AABB，不能表达真实有向盒。
    auto boxPolyData = vtkSmartPointer<vtkPolyData>::New();
    m_representation->GetPolyData(boxPolyData);
    if (!boxPolyData->GetPoints() || boxPolyData->GetNumberOfPoints() < 8) {
        return false;
    }

    double currentP0[3] = { 0.0, 0.0, 0.0 };
    double currentP1[3] = { 0.0, 0.0, 0.0 };
    double currentP3[3] = { 0.0, 0.0, 0.0 };
    double currentP4[3] = { 0.0, 0.0, 0.0 };
    boxPolyData->GetPoint(0, currentP0);
    boxPolyData->GetPoint(1, currentP1);
    boxPolyData->GetPoint(3, currentP3);
    boxPolyData->GetPoint(4, currentP4);

    // 用 P0/P1/P3/P4 反解 initialWorld -> currentWorld affine：
    // currentP0 是基准盒 minX/minY/minZ 角点交互后的 world 位置，
    // currentP1/currentP3/currentP4 分别提供基准盒 X/Y/Z 三条边的当前 world 方向和长度，
    // 因此矩阵能表达用户拖拽后的旋转、缩放和平移，而不是只表达外接框大小。
    auto baseToNowMat = vtkSmartPointer<vtkMatrix4x4>::New();
    baseToNowMat->Identity();
    for (int row = 0; row < 3; ++row) {
        // 每条当前 world 边除以基准盒原始 world 尺寸，得到 initialWorld 单位长度在 currentWorld 中的步长。
        const double baseToNowX =
            (currentP1[row] - currentP0[row]) / baseSize[0];
        const double baseToNowY =
            (currentP3[row] - currentP0[row]) / baseSize[1];
        const double baseToNowZ =
            (currentP4[row] - currentP0[row]) / baseSize[2];

        // 反解 offset，让基准盒 min 角点精确映射到 currentP0；
        // 这样矩阵同时包含原始 min 坐标补偿，不会把基准盒错误地当成从原点开始。
        const double baseOffset =
            currentP0[row]
            - baseToNowX * m_baseBounds[0]
            - baseToNowY * m_baseBounds[2]
            - baseToNowZ * m_baseBounds[4];

        baseToNowMat->SetElement(row, 0, baseToNowX);
        baseToNowMat->SetElement(row, 1, baseToNowY);
        baseToNowMat->SetElement(row, 2, baseToNowZ);
        baseToNowMat->SetElement(row, 3, baseOffset);
    }

    vtkMatrix4x4::DeepCopy(baseToNow.data(), baseToNowMat);
    return true;
}

void CropBoxWidget::Impl::SetBoundsCallback(BoundsCallback callback)
{
    m_boundsCallback = std::move(callback);
}

bool CropBoxWidget::Impl::SetEnabled(bool isEnabled)
{
    // 启用 widget 必须先绑定 interactor；
    // 没有窗口事件源时直接返回失败，避免创建半启用状态。
    if (isEnabled && !m_interactor) {
        return false;
    }

    // observer 懒绑定且只绑定一次；
    // 重复进入裁切模式时不应产生多重 VTK 回调。
    AttachObservers();

    if (isEnabled) {
        // 启用前确保有合法 world bounds；
        // 当前盒无效时回退到 reference world bounds，保证 PlaceWidget 有稳定输入。
        if (!GetBoundsAreValid(m_currentWorldBounds)) {
            m_currentWorldBounds = m_referenceWorldBounds;
        }

        if (!GetBoundsAreValid(m_currentWorldBounds)) {
            return false;
        }

        m_baseBounds = m_currentWorldBounds;
        m_representation->PlaceWidget(m_currentWorldBounds.data());
        SetVisualState(CropInteractionPhase::Idle);
        m_widget->On();
    }
    else {
        m_widget->Off();
        SetVisualState(CropInteractionPhase::Idle);
    }

    m_isEnabled = isEnabled;
    return true;
}

bool CropBoxWidget::Impl::GetEnabled() const
{
    return m_isEnabled;
}

void CropBoxWidget::Impl::SendWidgetEvent(
    vtkObject* caller,
    unsigned long eventId,
    void* clientData,
    void* callData)
{
    (void)caller;
    (void)callData;

    auto* widget = static_cast<CropBoxWidget::Impl*>(clientData);
    if (widget) {
        widget->OnWidgetEvent(eventId);
    }
}

bool CropBoxWidget::Impl::GetBoundsAreValid(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

CropInteractionPhase CropBoxWidget::Impl::GetEventPhase(unsigned long eventId)
{
    // Start/Interaction 一律视为拖拽中，只有 End 才切换到 Released。
    switch (eventId) {
    case vtkCommand::StartInteractionEvent:
        return CropInteractionPhase::Dragging;
    case vtkCommand::InteractionEvent:
        return CropInteractionPhase::Dragging;
    case vtkCommand::EndInteractionEvent:
        return CropInteractionPhase::Released;
    default:
        return CropInteractionPhase::Idle;
    }
}

void CropBoxWidget::Impl::AttachObservers()
{
    if (m_hasObservers) {
        return;
    }

    // observer 只绑定一次，避免重复进入模式时收到多重回调。
    m_observerTags = {
        m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand),
        m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand),
        m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand)
    };
    m_hasObservers = true;
}

void CropBoxWidget::Impl::OnWidgetEvent(unsigned long eventId)
{
    const auto interactionPhase = GetEventPhase(eventId);
    SetVisualState(interactionPhase);

    const auto rawBounds = m_representation->GetBounds();
    if (!rawBounds) {
        return;
    }

    // GetBounds 只返回当前 widget 的 world AABB；
    // 完整旋转姿态由 GetPolyData 重建 initialWorldToCurrentWorld，避免把外接框误当作有向盒。
    const std::array<double, 6> worldBounds = {
        rawBounds[0], rawBounds[1],
        rawBounds[2], rawBounds[3],
        rawBounds[4], rawBounds[5]
    };
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_currentWorldBounds = worldBounds;
    if (m_boundsCallback) {
        // 这里不做任何 preview / 算法调用，只把纯状态变化上抛给交互桥。
        // 因此 dragging 与 released 的不同处理策略，完全由桥接层决定。
        m_boundsCallback(m_currentWorldBounds, interactionPhase);
    }
}

void CropBoxWidget::Impl::SetVisualState(CropInteractionPhase phase)
{
    const bool isDragging = phase == CropInteractionPhase::Dragging;

    // 1. 普通 outline 在单面拖动时仍会被 VTK 使用；
    //    因此拖动期间要临时改普通 outline，才能覆盖“拖面只亮面、不亮线”的默认行为。
    // 2. selected outline 仍保留高亮色；
    //    平移/缩放等 VTK 会直接切到 selected outline 的场景，也能使用同一套交互颜色。
    m_representation->GetOutlineProperty()->SetColor(
        isDragging ? kBoxOutlineSelectedRed : kBoxOutlineNormalRed,
        isDragging ? kBoxOutlineSelectedGreen : kBoxOutlineNormalGreen,
        isDragging ? kBoxOutlineSelectedBlue : kBoxOutlineNormalBlue);
    m_representation->GetOutlineProperty()->SetLineWidth(
        isDragging ? kBoxHotWidth : kBoxLineWidth);
    m_representation->GetSelectedOutlineProperty()->SetColor(
        kBoxOutlineSelectedRed,
        kBoxOutlineSelectedGreen,
        kBoxOutlineSelectedBlue);
    m_representation->GetSelectedOutlineProperty()->SetLineWidth(kBoxHotWidth);
}

CropBoxWidget::CropBoxWidget()
    : m_impl(std::make_unique<CropBoxWidget::Impl>())
{
}

CropBoxWidget::~CropBoxWidget() = default;

CropBoxWidget::CropBoxWidget(CropBoxWidget&&) noexcept = default;

CropBoxWidget& CropBoxWidget::operator=(CropBoxWidget&&) noexcept = default;

void CropBoxWidget::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    m_impl->SetInteractor(interactor);
}

void CropBoxWidget::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    m_impl->SetReferenceWorldBounds(worldBounds);
}

void CropBoxWidget::SetWidgetWorldBounds(const std::array<double, 6>& worldBounds)
{
    m_impl->SetWidgetWorldBounds(worldBounds);
}

const std::array<double, 6>& CropBoxWidget::GetCurrentWorldBounds() const
{
    return m_impl->GetCurrentWorldBounds();
}

bool CropBoxWidget::GetCurrentWorldBox(
    CropVectorDouble3Array& baseCenter,
    CropVectorDouble3Array& baseSize,
    CropMatrixDouble16Array& baseToNow) const
{
    return m_impl->GetCurrentWorldBox(baseCenter, baseSize, baseToNow);
}

void CropBoxWidget::SetBoundsCallback(BoundsCallback callback)
{
    m_impl->SetBoundsCallback(std::move(callback));
}

bool CropBoxWidget::SetEnabled(bool isEnabled)
{
    return m_impl->SetEnabled(isEnabled);
}

bool CropBoxWidget::GetEnabled() const
{
    return m_impl->GetEnabled();
}
