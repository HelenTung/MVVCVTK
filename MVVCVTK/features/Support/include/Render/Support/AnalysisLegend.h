#pragma once
#include "AnalysisColorStyle.h"
#include <sstream>
#include <vtkActor2D.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkTextActor.h>
#include <vtkUnsignedCharArray.h>
#include <vtkViewport.h>

// 连续色带复用 VTK；精确分段按真实坐标画色块，亚像素段保留边界线与读数。
class AnalysisLegend final : public vtkScalarBarActor {
  public:
    static AnalysisLegend *New() { return new AnalysisLegend; }
    vtkTypeMacro(AnalysisLegend, vtkScalarBarActor);
    void SetMapping(const std::array<double, 2> &range, AnalysisColorStyle::RampParams params,
                    bool hasValues = true) {
        AnalysisColorStyle::SetTailColors(params);
        m_range = range;
        m_params = std::move(params);
        m_hasValues = hasValues;
        m_exact = !m_hasValues || range[0] == range[1] || !m_params.segments.empty() ||
                  m_params.tolerance.has_value();
        Modified();
    }
    const std::array<double, 2> &GetMappingRange() const { return m_range; }
    const AnalysisColorStyle::RampParams &GetMapping() const { return m_params; }
    int RenderOpaqueGeometry(vtkViewport *v) override {
        if (!GetIsCustom(v))
            return Superclass::RenderOpaqueGeometry(v);
        const auto *size = v->GetSize();
        if (m_stamp < GetMTime() || m_size != std::array<int, 2>{size[0], size[1]})
            Build(v);
        int rendered = m_bar->RenderOpaqueGeometry(v);
        for (auto &text : m_text)
            rendered += text->RenderOpaqueGeometry(v);
        return rendered;
    }
    int RenderTranslucentPolygonalGeometry(vtkViewport *v) override {
        return GetIsCustom(v) ? 0 : Superclass::RenderTranslucentPolygonalGeometry(v);
    }
    int RenderOverlay(vtkViewport *v) override {
        if (!GetIsCustom(v))
            return Superclass::RenderOverlay(v);
        const auto *size = v->GetSize();
        if (m_stamp < GetMTime() || m_size != std::array<int, 2>{size[0], size[1]})
            Build(v);
        int rendered = m_bar->RenderOverlay(v);
        for (auto &text : m_text)
            rendered += text->RenderOverlay(v);
        return rendered;
    }
    void ReleaseGraphicsResources(vtkWindow *w) override {
        Superclass::ReleaseGraphicsResources(w);
        m_bar->ReleaseGraphicsResources(w);
        for (auto &text : m_text)
            text->ReleaseGraphicsResources(w);
    }

  private:
    AnalysisLegend() { m_bar = vtkSmartPointer<vtkActor2D>::New(); }
    bool GetIsCustom(vtkViewport *v) {
        const std::string title = GetTitle() ? GetTitle() : "";
        const auto lines = 1 + std::count(title.begin(), title.end(), '\n');
        return m_exact || v->GetSize()[1] < 38 + 28 + 13 * lines + 24 + 8;
    }
    void Build(vtkViewport *v) {
        const auto *size = v->GetSize();
        m_size = {size[0], size[1]};
        const auto *position = GetPosition();
        const double x = position[0] * size[0];
        const std::string title = GetTitle() ? GetTitle() : "";
        const auto titleLines = 1 + std::count(title.begin(), title.end(), '\n');
        const bool compact = size[1] < 38 + 28 + 13 * titleLines + 24 + 8;
        const double y = compact ? 28 : std::max(42., position[1] * size[1]);
        const double height =
            std::max(8., std::min({GetHeight() * size[1], double(GetMaximumHeightInPixels()),
                                   size[1] - y - 13 * titleLines - 44.}));
        auto points = vtkSmartPointer<vtkPoints>::New();
        auto cells = vtkSmartPointer<vtkCellArray>::New();
        auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
        colors->SetNumberOfComponents(4);
        m_text.clear();
        auto text = [&](const std::string &label, double tx, double ty) {
            auto a = vtkSmartPointer<vtkTextActor>::New();
            a->SetInput(label.c_str());
            a->SetPosition(tx, ty);
            a->GetTextProperty()->ShallowCopy(GetLabelTextProperty());
            a->GetTextProperty()->SetFontSize(compact ? 9 : 11);
            m_text.push_back(a);
        };
        text(title, x, compact ? size[1] - 13 * titleLines - 2 : y + height + 38);
        auto quad = [&](double left, double right, double low, double high,
                        const std::array<double, 4> &c) {
            vtkIdType ids[4];
            ids[0] = points->InsertNextPoint(left, low, 0);
            ids[1] = points->InsertNextPoint(right, low, 0);
            ids[2] = points->InsertNextPoint(right, high, 0);
            ids[3] = points->InsertNextPoint(left, high, 0);
            cells->InsertNextCell(4, ids);
            unsigned char rgba[4];
            for (int k = 0; k < 4; ++k)
                rgba[k] = static_cast<unsigned char>(std::lround(255 * c[k]));
            colors->InsertNextTypedTuple(rgba);
        };
        const auto box = [&](double low, double high, const std::array<double, 4> &c) {
            quad(x, x + 12, low, high, c);
        };
        std::vector<double> bounds{m_range[0], m_range[1]};
        for (const auto &s : m_params.segments)
            for (auto b : {s.lower, s.upper})
                if (b && *b > m_range[0] && *b < m_range[1])
                    bounds.push_back(*b);
        if (m_params.tolerance)
            for (double b : *m_params.tolerance)
                if (b > m_range[0] && b < m_range[1])
                    bounds.push_back(b);
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
        if (compact) {
            // 矮视窗横排端帽与有限段，不用给无界尾段伪造数值长度。
            const double left = x + 16, width = std::max(8., size[0] - x - 40.);
            if (m_hasValues) {
                quad(x, x + 12, y, y + 10,
                     {m_params.belowColor[0], m_params.belowColor[1], m_params.belowColor[2], 1});
                text(u8"−∞", x, y - 12);
                quad(left + width + 4, left + width + 16, y, y + 10,
                     {m_params.aboveColor[0], m_params.aboveColor[1], m_params.aboveColor[2], 1});
                text(u8"+∞", left + width, y - 12);
                if (m_range[0] == m_range[1]) {
                    quad(left, left + width, y, y + 10,
                         AnalysisColorStyle::GetMappedColor(m_range, m_params, m_range[0]));
                    std::ostringstream label;
                    label << u8"单值 " << m_range[0];
                    text(label.str(), left, y - 12);
                } else {
                    for (std::size_t i = 1; i < bounds.size(); ++i)
                        for (int k = 0; k < 32; ++k) {
                            const double a = (1 - k / 32.0) * bounds[i - 1] +
                                             (k / 32.0) * bounds[i],
                                         b = (1 - (k + 1) / 32.0) * bounds[i - 1] +
                                             ((k + 1) / 32.0) * bounds[i];
                            quad(left + width * AnalysisColorStyle::GetFraction(m_range[0],
                                                                                m_range[1], a),
                                 left + width * AnalysisColorStyle::GetFraction(m_range[0],
                                                                                m_range[1], b),
                                 y, y + 10,
                                 AnalysisColorStyle::GetMappedColor(m_range, m_params,
                                                                    a * .5 + b * .5));
                        }
                    std::ostringstream low, high;
                    low << m_range[0];
                    high << m_range[1];
                    text(low.str(), left, y - 12);
                    text(high.str(), left + width - 24, y - 12);
                    for (double boundary : bounds) {
                        const double bx = left + width * AnalysisColorStyle::GetFraction(
                                                             m_range[0], m_range[1], boundary);
                        quad(bx - .25, bx + .25, y - 2, y + 12, {1, 1, 1, 1});
                    }
                }
            } else
                text(u8"暂无有效测量值", x, y);
            quad(x, x + 10, 0, 10,
                 {m_params.invalidColor[0], m_params.invalidColor[1], m_params.invalidColor[2], 1});
            text(u8"无效／未测", x + 14, 0);
        } else if (!m_hasValues) {
            box(y, y + 16, {.5, .5, .5, 1});
            text(u8"暂无有效测量值", x + 18, y);
        } else if (m_range[0] == m_range[1]) {
            box(y, y + 16, AnalysisColorStyle::GetMappedColor(m_range, m_params, m_range[0]));
            std::ostringstream label;
            label << u8"单值 " << m_range[0];
            text(label.str(), x + 18, y);
            box(y - 20, y - 8,
                {m_params.belowColor[0], m_params.belowColor[1], m_params.belowColor[2], 1});
            text(u8"下侧无界段 → −∞", x + 18, y - 20);
            box(y + 20, y + 32,
                {m_params.aboveColor[0], m_params.aboveColor[1], m_params.aboveColor[2], 1});
            text(u8"上侧无界段 → +∞", x + 18, y + 20);
        } else {
            // 每个实际边界都成为几何顶点；连续段内部采样不会吞掉窄恒定段。
            for (std::size_t i = 1; i < bounds.size(); ++i)
                for (int k = 0; k < 32; ++k) {
                    const double a = (1 - k / 32.0) * bounds[i - 1] + (k / 32.0) * bounds[i];
                    const double b =
                        (1 - (k + 1) / 32.0) * bounds[i - 1] + ((k + 1) / 32.0) * bounds[i];
                    box(y + height * AnalysisColorStyle::GetFraction(m_range[0], m_range[1], a),
                        y + height * AnalysisColorStyle::GetFraction(m_range[0], m_range[1], b),
                        AnalysisColorStyle::GetMappedColor(m_range, m_params, a * .5 + b * .5));
                }
            double previousLabel = -1e30;
            for (double b : bounds) {
                const double by =
                    y + height * AnalysisColorStyle::GetFraction(m_range[0], m_range[1], b);
                const double labelY = std::max(by, previousLabel + 13);
                previousLabel = labelY;
                std::ostringstream label;
                label.precision(8);
                label << b;
                if (labelY != by)
                    label << u8" ↙";
                box(by - .25, by + .25, {1, 1, 1, 1});
                text(label.str(), x + 18, labelY);
            }
            box(y - 20, y - 8,
                {m_params.belowColor[0], m_params.belowColor[1], m_params.belowColor[2], 1});
            text(u8"下侧无界段 → −∞", x + 18, y - 20);
            box(y + height + 18, y + height + 30,
                {m_params.aboveColor[0], m_params.aboveColor[1], m_params.aboveColor[2], 1});
            text(u8"上侧无界段 → +∞", x + 18, y + height + 18);
        }
        if (!compact)
            box(y - 38, y - 26,
                {m_params.invalidColor[0], m_params.invalidColor[1], m_params.invalidColor[2], 1});
        if (!compact)
            text(u8"无效／未测", x + 18, y - 38);
        auto poly = vtkSmartPointer<vtkPolyData>::New();
        poly->SetPoints(points);
        poly->SetPolys(cells);
        poly->GetCellData()->SetScalars(colors);
        auto mapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
        mapper->SetInputData(poly);
        mapper->SetColorModeToDirectScalars();
        m_bar->SetMapper(mapper);
        m_stamp = GetMTime();
    }
    std::array<double, 2> m_range{};
    AnalysisColorStyle::RampParams m_params;
    bool m_hasValues = true, m_exact = false;
    vtkMTimeType m_stamp = 0;
    std::array<int, 2> m_size{};
    vtkSmartPointer<vtkActor2D> m_bar;
    std::vector<vtkSmartPointer<vtkTextActor>> m_text;
};
