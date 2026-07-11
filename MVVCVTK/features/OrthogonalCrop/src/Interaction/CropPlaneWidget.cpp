// =====================================================================
// Path: MVVCVTK/features/OrthogonalCrop/src/Interaction/CropPlaneWidget.cpp
// 分类: Service / Widget Controller Implementation
// 说明: 封装 vtkImplicitPlaneWidget2 的构造、启停、observer 绑定与平面回调翻译。
// =====================================================================

#include "Interaction/CropPlaneWidget.h"

#include <vtkCallbackCommand.h>
#include <vtkCommand.h>
#include <vtkImplicitPlaneRepresentation.h>
#include <vtkImplicitPlaneWidget2.h>
#include <vtkMath.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <memory>
#include <utility>

static constexpr double kPlaneWidgetNormalEpsilon = 1e-8;
static constexpr double kPlaneColorRed = 0.1;
static constexpr double kPlaneColorGreen = 0.85;
static constexpr double kPlaneColorBlue = 0.5;
static constexpr double kPlaneSelectedRed = 1.0;
static constexpr double kPlaneSelectedGreen = 0.55;
static constexpr double kPlaneSelectedBlue = 0.12;
static constexpr double kPlaneOpacity = 0.28;
static constexpr double kPlaneSelectedOpacity = 0.36;
static constexpr double kPlaneLineWidth = 2.0;

class CropPlaneWidget::Impl final {
public:
    using PlaneCallback = CropPlaneWidget::PlaneCallback;

    Impl();
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void SetInteractor(vtkRenderWindowInteractor* interactor);
    void SetReferenceWorldBounds(const std::array<double, 6>& worldBounds);
    void SetWidgetWorldPlane(
        const CropVectorDouble3Array& worldOrigin,
        const CropVectorDouble3Array& worldNormal,
        const std::array<double, 2>& worldHalfExtents);
    bool GetCurrentWorldPlane(
        CropVectorDouble3Array& worldOrigin,
        CropVectorDouble3Array& worldNormal) const;
    void SetPlaneCallback(PlaneCallback callback);
    bool SetEnabled(bool isEnabled);
    bool GetEnabled() const;

private:
    static void SendWidgetEvent(
        vtkObject* caller,
        unsigned long eventId,
        void* clientData,
        void* callData);
    static bool GetBoundsAreValid(const std::array<double, 6>& bounds);
    static bool SetUnitNormal(CropVectorDouble3Array& worldNormal);
    static CropInteractionPhase GetEventPhase(unsigned long eventId);

    void AttachObservers();
    void SetPlaneRep();
    void OnWidgetEvent(unsigned long eventId);

    // 非拥有事件源；SetInteractor 注入，启用期间必须有效，Impl 不负责删除。
    vtkRenderWindowInteractor* m_interactor = nullptr;
    // Impl 持有 widget/representation 的 VTK 引用；widget 同时引用 representation。
    vtkSmartPointer<vtkImplicitPlaneWidget2> m_widget;
    vtkSmartPointer<vtkImplicitPlaneRepresentation> m_representation;
    // Impl 持有 VTK 回调命令；clientData 指回 this，析构时先清空以切断悬空回调入口。
    vtkSmartPointer<vtkCallbackCommand> m_callbackCommand;
    // 上层回调值副本；VTK 事件线程同步消费，SetPlaneCallback 替换。
    PlaneCallback m_planeCallback;
    // 最近一次成功 On/Off 后的逻辑状态；启用前置校验失败时不改写。
    bool m_isEnabled = false;
    // observer 只懒绑定一次；tag 由 AddObserver 生产，Impl 析构逐项 RemoveObserver。
    bool m_hasObservers = false;
    std::array<unsigned long, 3> m_observerTags = { 0, 0, 0 };
    // 调用方提供的 world AABB；决定初始中心并限定 representation 的 PlaceWidget 范围。
    std::array<double, 6> m_referenceWorldBounds = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // 当前 world 平面真源；origin 由 setter/交互更新，normal 始终在写入时归一化。
    CropVectorDouble3Array m_currentWorldOrigin = { 0.0, 0.0, 0.0 };
    CropVectorDouble3Array m_currentWorldNormal = { 0.0, 0.0, 1.0 };
    // widget 可视半尺寸 [halfWidth, halfHeight]；只决定 PlaceWidget 范围，不限制无限平面裁切。
    std::array<double, 2> m_currentWorldHalfExtents = { 1.0, 1.0 };
};

CropPlaneWidget::Impl::Impl()
{
    // 平面 widget 外观固定在控制器内，上层 bridge 只消费 world 中心点/法线。
    m_widget = vtkSmartPointer<vtkImplicitPlaneWidget2>::New();
    m_representation = vtkSmartPointer<vtkImplicitPlaneRepresentation>::New();
    m_representation->SetPlaceFactor(1.0);
    m_representation->DrawPlaneOn();
    m_representation->SetCropPlaneToBoundingBox(false);
    m_representation->DrawOutlineOff();
    m_representation->OutlineTranslationOff();
    m_representation->ScaleEnabledOff();
    m_representation->ConstrainToWidgetBoundsOff();
    m_representation->TubingOff();
    m_representation->GetPlaneProperty()->SetColor(
        kPlaneColorRed,
        kPlaneColorGreen,
        kPlaneColorBlue);
    m_representation->GetPlaneProperty()->SetOpacity(kPlaneOpacity);
    m_representation->GetOutlineProperty()->SetColor(
        kPlaneColorRed,
        kPlaneColorGreen,
        kPlaneColorBlue);
    m_representation->GetOutlineProperty()->SetLineWidth(kPlaneLineWidth);
    m_representation->GetSelectedPlaneProperty()->SetColor(
        kPlaneSelectedRed,
        kPlaneSelectedGreen,
        kPlaneSelectedBlue);
    m_representation->GetSelectedPlaneProperty()->SetOpacity(kPlaneSelectedOpacity);
    m_widget->SetRepresentation(m_representation);

    m_callbackCommand = vtkSmartPointer<vtkCallbackCommand>::New();
    m_callbackCommand->SetClientData(this);
    m_callbackCommand->SetCallback(&CropPlaneWidget::Impl::SendWidgetEvent);
}

CropPlaneWidget::Impl::~Impl()
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

void CropPlaneWidget::Impl::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    m_interactor = interactor;
    m_widget->SetInteractor(interactor);
}

void CropPlaneWidget::Impl::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    if (!GetBoundsAreValid(worldBounds)) {
        return;
    }

    m_referenceWorldBounds = worldBounds;
    m_currentWorldOrigin = {
        (worldBounds[0] + worldBounds[1]) * 0.5,
        (worldBounds[2] + worldBounds[3]) * 0.5,
        (worldBounds[4] + worldBounds[5]) * 0.5
    };
    SetPlaneRep();
}

void CropPlaneWidget::Impl::SetWidgetWorldPlane(
    const CropVectorDouble3Array& worldOrigin,
    const CropVectorDouble3Array& worldNormal,
    const std::array<double, 2>& worldHalfExtents)
{
    auto normalizedNormal = worldNormal;
    if (!SetUnitNormal(normalizedNormal)) {
        return;
    }

    m_currentWorldOrigin = worldOrigin;
    m_currentWorldNormal = normalizedNormal;
    m_currentWorldHalfExtents = {
        std::max(worldHalfExtents[0], kPlaneWidgetNormalEpsilon),
        std::max(worldHalfExtents[1], kPlaneWidgetNormalEpsilon)
    };
    SetPlaneRep();
}

bool CropPlaneWidget::Impl::GetCurrentWorldPlane(
    CropVectorDouble3Array& worldOrigin,
    CropVectorDouble3Array& worldNormal) const
{
    worldOrigin = m_currentWorldOrigin;
    worldNormal = m_currentWorldNormal;
    return vtkMath::Norm(worldNormal.data()) > kPlaneWidgetNormalEpsilon;
}

void CropPlaneWidget::Impl::SetPlaneCallback(PlaneCallback callback)
{
    m_planeCallback = std::move(callback);
}

bool CropPlaneWidget::Impl::SetEnabled(bool isEnabled)
{
    if (isEnabled && !m_interactor) {
        return false;
    }

    AttachObservers();

    if (isEnabled) {
        if (!GetBoundsAreValid(m_referenceWorldBounds)) {
            return false;
        }

        SetPlaneRep();
        m_widget->On();
    }
    else {
        m_widget->Off();
    }

    m_isEnabled = isEnabled;
    return true;
}

bool CropPlaneWidget::Impl::GetEnabled() const
{
    return m_isEnabled;
}

void CropPlaneWidget::Impl::SendWidgetEvent(
    vtkObject* caller,
    unsigned long eventId,
    void* clientData,
    void* callData)
{
    (void)caller;
    (void)callData;

    auto* widget = static_cast<CropPlaneWidget::Impl*>(clientData);
    if (widget) {
        widget->OnWidgetEvent(eventId);
    }
}

bool CropPlaneWidget::Impl::GetBoundsAreValid(const std::array<double, 6>& bounds)
{
    return bounds[0] < bounds[1]
        && bounds[2] < bounds[3]
        && bounds[4] < bounds[5];
}

bool CropPlaneWidget::Impl::SetUnitNormal(CropVectorDouble3Array& worldNormal)
{
    const double length = vtkMath::Norm(worldNormal.data());
    if (length <= kPlaneWidgetNormalEpsilon) {
        return false;
    }

    for (double& component : worldNormal) {
        component /= length;
    }
    return true;
}

CropInteractionPhase CropPlaneWidget::Impl::GetEventPhase(unsigned long eventId)
{
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

void CropPlaneWidget::Impl::AttachObservers()
{
    if (m_hasObservers) {
        return;
    }

    m_observerTags = {
        m_widget->AddObserver(vtkCommand::StartInteractionEvent, m_callbackCommand),
        m_widget->AddObserver(vtkCommand::InteractionEvent, m_callbackCommand),
        m_widget->AddObserver(vtkCommand::EndInteractionEvent, m_callbackCommand)
    };
    m_hasObservers = true;
}

void CropPlaneWidget::Impl::SetPlaneRep()
{
    if (!m_representation || !GetBoundsAreValid(m_referenceWorldBounds)) {
        return;
    }

    // VTK 的平面控件只能通过 PlaceWidget bounds 推导显示面大小；
    // halfExtents 已由 bridge/request 状态给出，这里只把它翻译成围绕当前 origin 的最小可视 AABB。
    const double visualHalfExtent = std::max(
        m_currentWorldHalfExtents[0],
        m_currentWorldHalfExtents[1]);
    std::array<double, 6> visualWorldBounds = {
        m_currentWorldOrigin[0] - visualHalfExtent,
        m_currentWorldOrigin[0] + visualHalfExtent,
        m_currentWorldOrigin[1] - visualHalfExtent,
        m_currentWorldOrigin[1] + visualHalfExtent,
        m_currentWorldOrigin[2] - visualHalfExtent,
        m_currentWorldOrigin[2] + visualHalfExtent
    };
    m_representation->PlaceWidget(visualWorldBounds.data());
    m_representation->SetOrigin(m_currentWorldOrigin.data());
    m_representation->SetNormal(m_currentWorldNormal.data());
}

void CropPlaneWidget::Impl::OnWidgetEvent(unsigned long eventId)
{
    if (!m_representation) {
        return;
    }

    double rawOrigin[3] = { 0.0, 0.0, 0.0 };
    double rawNormal[3] = { 0.0, 0.0, 1.0 };
    m_representation->GetOrigin(rawOrigin);
    m_representation->GetNormal(rawNormal);

    CropVectorDouble3Array worldOrigin = { rawOrigin[0], rawOrigin[1], rawOrigin[2] };
    CropVectorDouble3Array worldNormal = { rawNormal[0], rawNormal[1], rawNormal[2] };
    if (!SetUnitNormal(worldNormal)) {
        return;
    }

    m_currentWorldOrigin = worldOrigin;
    m_currentWorldNormal = worldNormal;
    if (m_planeCallback) {
        m_planeCallback(
            m_currentWorldOrigin,
            m_currentWorldNormal,
            GetEventPhase(eventId));
    }
}

CropPlaneWidget::CropPlaneWidget()
    : m_impl(std::make_unique<CropPlaneWidget::Impl>())
{
}

CropPlaneWidget::~CropPlaneWidget() = default;

CropPlaneWidget::CropPlaneWidget(CropPlaneWidget&&) noexcept = default;

CropPlaneWidget& CropPlaneWidget::operator=(CropPlaneWidget&&) noexcept = default;

void CropPlaneWidget::SetInteractor(vtkRenderWindowInteractor* interactor)
{
    m_impl->SetInteractor(interactor);
}

void CropPlaneWidget::SetReferenceWorldBounds(const std::array<double, 6>& worldBounds)
{
    m_impl->SetReferenceWorldBounds(worldBounds);
}

void CropPlaneWidget::SetWidgetWorldPlane(
    const CropVectorDouble3Array& worldOrigin,
    const CropVectorDouble3Array& worldNormal,
    const std::array<double, 2>& worldHalfExtents)
{
    m_impl->SetWidgetWorldPlane(worldOrigin, worldNormal, worldHalfExtents);
}

bool CropPlaneWidget::GetCurrentWorldPlane(
    CropVectorDouble3Array& worldOrigin,
    CropVectorDouble3Array& worldNormal) const
{
    return m_impl->GetCurrentWorldPlane(worldOrigin, worldNormal);
}

void CropPlaneWidget::SetPlaneCallback(PlaneCallback callback)
{
    m_impl->SetPlaneCallback(std::move(callback));
}

bool CropPlaneWidget::SetEnabled(bool isEnabled)
{
    return m_impl->SetEnabled(isEnabled);
}

bool CropPlaneWidget::GetEnabled() const
{
    return m_impl->GetEnabled();
}
