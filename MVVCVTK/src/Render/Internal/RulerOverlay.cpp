#include "Render/Internal/RulerOverlay.h"

#include <vtkActor2D.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCoordinate.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProp.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkWeakPointer.h>

#include <algorithm>
#include <cmath>

class RulerOverlay::Impl final {
public:
    // 无世界 bounds，也不修改 camera。RenderOverlay 在本次投影建立后调用。
    class RulerProp final : public vtkProp {
    public:
        static RulerProp* New() { return new RulerProp; }
        vtkTypeMacro(RulerProp, vtkProp);
        Impl* owner = nullptr;
        int RenderOverlay(vtkViewport* viewport) override {
            return owner ? owner->OnRender(vtkRenderer::SafeDownCast(viewport)) : 0;
        }
        void ReleaseGraphicsResources(vtkWindow* window) override {
            if (owner) {
                owner->line->ReleaseGraphicsResources(window);
                owner->text->ReleaseGraphicsResources(window);
            }
        }
    };

    Impl() {
        prop->owner = this;
        prop->PickableOff();
        points->SetDataTypeToDouble();
        points->SetNumberOfPoints(8);
        auto cells = vtkSmartPointer<vtkCellArray>::New();
        for (vtkIdType index = 0; index < 8; index += 2) {
            const vtkIdType ids[2]{ index, index + 1 };
            cells->InsertNextCell(2, ids);
        }
        poly->SetPoints(points);
        poly->SetLines(cells);
        auto mapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
        mapper->SetInputData(poly);
        line->SetMapper(mapper);
        line->GetProperty()->SetLineWidth(2.0);
        line->PickableOff();
        text->PickableOff();
        text->GetPositionCoordinate()->SetCoordinateSystemToViewport();
        text->GetTextProperty()->SetFontFamilyToArial();
        text->GetTextProperty()->SetJustificationToCentered();
        text->GetTextProperty()->SetVerticalJustificationToBottom();
        text->GetTextProperty()->ShadowOn();
    }

    int OnRender(vtkRenderer* view) {
        state = {};
        state.dataRevision = input.dataRevision;
        state.bindingRevision = input.bindingRevision;
        if (!input.hasData) return 0;
        if (!input.isVisible) { state.status = RulerStatus::Hidden; return 0; }
        if (!RulerMetrics::GetGeometryValid(input.geometry)) {
            state.status = RulerStatus::InvalidGeometry; return 0;
        }
        std::array<double, 9> inverse{};
        if (!RulerMetrics::GetInverse(input.modelToWorld, inverse)) {
            state.status = RulerStatus::InvalidTransform; return 0;
        }
        auto* camera = view ? view->GetActiveCamera() : nullptr;
        if (!camera || !camera->GetParallelProjection()) {
            state.status = RulerStatus::UnsupportedProjection; return 0;
        }
        // ExplicitProjectionTransformMatrix 可覆盖 ParallelProjection 标志；必须确认
        // 实际投影的齐次 w 不依赖 world 位置，才能把一把标尺用于整个平行视平面。
        auto* projection = camera->GetCompositeProjectionTransformMatrix(view->GetTiledAspectRatio(), 0.0, 1.0);
        if (!projection || projection->GetElement(3,0) != 0.0 || projection->GetElement(3,1) != 0.0
            || projection->GetElement(3,2) != 0.0 || !std::isfinite(projection->GetElement(3,3))
            || projection->GetElement(3,3) == 0.0) {
            state.status = RulerStatus::UnsupportedProjection; return 0;
        }
        int axis = -1;
        switch (input.mode) {
        case VizMode::SliceTop_down: axis = 2; break;
        case VizMode::SliceFront_back: axis = 1; break;
        case VizMode::SliceLeft_right: axis = 0; break;
        default: break;
        }
        if (axis >= 0 && std::abs(camera->GetDirectionOfProjection()[axis]) < 1.0 - 1e-8) {
            state.status = RulerStatus::UnsupportedProjection; return 0;
        }
        const int* size = view->GetSize();
        const int width = size[0], height = size[1];
        const int* origin = view->GetOrigin();
        const int originX = origin[0], originY = origin[1];
        const double margin = std::max(16.0, static_cast<double>(params.fontSize));
        if (height < params.fontSize * 3 + 2 * margin) {
            state.status = RulerStatus::ViewportTooSmall; return 0;
        }
        // 使用完整输出 viewport（GetSize 包含 TileScale），每个 tile 由 VTK 裁剪。
        // display 反投影必须包含子 viewport 原点，二维几何仍采用 viewport 局部像素。
        auto* focal = camera->GetFocalPoint();
        view->SetWorldPoint(focal[0], focal[1], focal[2], 1.0);
        view->WorldToDisplay();
        const double depth = view->GetDisplayPoint()[2];
        std::array<std::array<double, 3>, 2> world{};
        for (int endpoint = 0; endpoint < 2; ++endpoint) {
            view->SetDisplayPoint(originX + width * 0.5 + endpoint * 100.0,
                originY + height * 0.5, depth);
            view->DisplayToWorld();
            auto* point = view->GetWorldPoint();
            if (!std::isfinite(point[3]) || point[3] == 0.0) {
                state.status = RulerStatus::InvalidScale; return 0;
            }
            for (int component = 0; component < 3; ++component) world[endpoint][component] = point[component] / point[3];
        }
        std::array<double, 3> delta{};
        for (int component = 0; component < 3; ++component) delta[component] = world[1][component] - world[0][component];
        int tileWidth = 0, tileHeight = 0, tileX = 0, tileY = 0;
        view->GetTiledSizeAndOrigin(&tileWidth, &tileHeight, &tileX, &tileY);
        if (tileWidth <= 0 || tileHeight <= 0) {
            state.status = RulerStatus::ViewportTooSmall; return 0;
        }
        // WindowToImageFilter 为当前 tile 缩放相机；DisplayToWorld 仍以完整输出宽度
        // 归一化。换回实际 tile 像素，避免把相机与 TileScale 的放大重复计算。
        const double mmPerPixel = RulerMetrics::GetLength(inverse, delta)
            * static_cast<double>(width) / tileWidth / 100.0;
        const double availablePixels = width - 2 * margin;
        if (mmPerPixel != lastScale || availablePixels != lastAvailable
            || params.targetPixels != lastTarget || params.unit != lastUnit) {
            metricState = RulerMetrics::BuildState(mmPerPixel, availablePixels, params, previousLengthMm);
            lastScale = mmPerPixel;
            lastAvailable = availablePixels;
            lastTarget = params.targetPixels;
            lastUnit = params.unit;
        }
        state = metricState;
        state.dataRevision = input.dataRevision;
        state.bindingRevision = input.bindingRevision;
        if (state.status != RulerStatus::Visible) return 0;
        previousLengthMm = state.lengthMm;
        const bool isRight = params.position == RulerPosition::BottomRight || params.position == RulerPosition::TopRight;
        const bool isTop = params.position == RulerPosition::TopLeft || params.position == RulerPosition::TopRight;
        const double startX = isRight ? width - margin - state.lengthPixels : margin;

        if (lastLabel != state.label) {
            text->SetInput(state.label.c_str());
            lastLabel = state.label;
        }
        text->GetTextProperty()->SetFontSize(params.fontSize);
        text->GetTextProperty()->SetColor(params.color.data());
        line->GetProperty()->SetColor(params.color.data());

        double textSize[2]{};
        text->GetSize(view, textSize);
        if (textSize[0] > width - 2 * margin || textSize[1] + 12.0 > height - 2 * margin) {
            state.status = RulerStatus::ViewportTooSmall;
            state.lengthMm = state.lengthPixels = 0.0;
            state.label.clear();
            return 0;
        }
        // VTK 截图可能按 TileScale 放大字体；用实际字体包围盒放置顶部标尺。
        const double startY = isTop ? height - margin - textSize[1] - 12.0 : margin;
        // 文字短于 viewport 但长于线条时仍保持在可视边界内。
        text->SetPosition(std::clamp(startX + state.lengthPixels * 0.5,
            margin + textSize[0] * 0.5, width - margin - textSize[0] * 0.5), startY + 8.0);
        const std::array<double, 3> layout{ startX, startY, state.lengthPixels };
        if (layout != lastLayout) {
            points->SetPoint(0, startX, startY, 0);
            points->SetPoint(1, startX + state.lengthPixels, startY, 0);
            for (int tick = 0; tick < 3; ++tick) {
                const double x = startX + tick * state.lengthPixels * 0.5;
                points->SetPoint(2 + tick * 2, x, startY - 4, 0);
                points->SetPoint(3 + tick * 2, x, startY + 4, 0);
            }
            points->Modified();
            lastLayout = layout;
        }
        return line->RenderOverlay(view) + text->RenderOverlay(view);
    }

    vtkWeakPointer<vtkRenderer> renderer;
    vtkSmartPointer<RulerProp> prop = vtkSmartPointer<RulerProp>::New();
    vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
    vtkSmartPointer<vtkPolyData> poly = vtkSmartPointer<vtkPolyData>::New();
    vtkSmartPointer<vtkActor2D> line = vtkSmartPointer<vtkActor2D>::New();
    vtkSmartPointer<vtkTextActor> text = vtkSmartPointer<vtkTextActor>::New();
    RulerInput input;
    RulerParams params;
    RulerState state;
    double previousLengthMm = 0.0;
    double lastScale = -1.0;
    double lastAvailable = -1.0;
    double lastTarget = -1.0;
    RulerUnit lastUnit = RulerUnit::Auto;
    RulerState metricState;
    std::string lastLabel;
    std::array<double, 3> lastLayout{ -1,-1,-1 };
};

RulerOverlay::RulerOverlay() : m_impl(std::make_unique<Impl>()) {}
RulerOverlay::~RulerOverlay() {
    DetachRenderer();
    m_impl->prop->owner = nullptr;
}
void RulerOverlay::AttachRenderer(vtkRenderer* renderer) {
    if (m_impl->renderer == renderer) return;
    // 先挂载候选；失败不卸载旧 renderer 的标尺。
    try {
        if (renderer) renderer->AddViewProp(m_impl->prop);
    } catch (...) {
        if (renderer) renderer->RemoveViewProp(m_impl->prop);
        throw;
    }
    if (m_impl->renderer) m_impl->renderer->RemoveViewProp(m_impl->prop);
    m_impl->renderer = renderer;
}
void RulerOverlay::DetachRenderer() {
    if (m_impl->renderer) m_impl->renderer->RemoveViewProp(m_impl->prop);
    m_impl->renderer = nullptr;
}
void RulerOverlay::SetInput(const RulerInput& input, const RulerParams& params) {
    m_impl->input = input;
    m_impl->params = params;
    m_impl->state = {};
    m_impl->state.status = input.hasData ? RulerStatus::Pending : RulerStatus::NoData;
    m_impl->state.dataRevision = input.dataRevision;
    m_impl->state.bindingRevision = input.bindingRevision;
    m_impl->prop->Modified();
}
const RulerState& RulerOverlay::GetState() const { return m_impl->state; }
void RulerOverlay::ClearInput() { SetInput({}, m_impl->params); }
