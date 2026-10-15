#pragma once

#include <vtkIntArray.h>
#include <vtkLookupTable.h>
#include <vtkObjectFactory.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

// Gap 私有分类色表。按区域编号建索引，避免 VTK 对每个面片/像素线性扫描注释。
// 正式标签和稀疏编号保持不变；空间占用只与区域数量有关。
class GapLabelLookupTable final : public vtkLookupTable {
public:
    static GapLabelLookupTable* New();
    vtkTypeMacro(GapLabelLookupTable, vtkLookupTable);

    void SetAnnotations(vtkAbstractArray* values, vtkStringArray* names) override {
        this->Superclass::SetAnnotations(values, names);
        RebuildIndex();
    }
    void DeepCopy(vtkScalarsToColors* source) override {
        this->Superclass::DeepCopy(source);
        RebuildIndex();
    }
    const unsigned char* MapValue(double value) override {
        if (!this->GetIndexedLookup() || !hasIndex_)
            return this->Superclass::MapValue(value);
        const auto index = Find(value);
        return index < 0 ? this->GetNanColorAsUnsignedChars() : this->GetPointer(index);
    }
    void GetColor(double value, double rgb[3]) override {
        if (!this->GetIndexedLookup() || !hasIndex_) {
            this->Superclass::GetColor(value, rgb); return;
        }
        const auto* color = MapValue(value);
        for (int i = 0; i < 3; ++i) rgb[i] = color[i] / 255.0;
    }
    double GetOpacity(double value) override {
        if (!this->GetIndexedLookup() || !hasIndex_)
            return this->Superclass::GetOpacity(value);
        return MapValue(value)[3] / 255.0;
    }
    void MapScalarsThroughTable2(void* input, unsigned char* output, int type,
        int count, int increment, int format) override {
        if (!this->GetIndexedLookup() || !hasIndex_
            || format < VTK_LUMINANCE || format > VTK_RGBA) {
            this->Superclass::MapScalarsThroughTable2(input, output, type, count, increment, format);
            return;
        }
        switch (type) {
            vtkTemplateMacro(MapRows(static_cast<const VTK_TT*>(input), output, count, increment, format));
        default:
            this->Superclass::MapScalarsThroughTable2(input, output, type, count, increment, format);
            break;
        }
    }

protected:
    GapLabelLookupTable() = default;
    ~GapLabelLookupTable() override = default;

private:
    void RebuildIndex() {
        indices_.clear();
        auto* values = vtkIntArray::SafeDownCast(this->GetAnnotatedValues());
        hasIndex_ = values != nullptr;
        if (!values) return;
        indices_.reserve(static_cast<std::size_t>(values->GetNumberOfValues()));
        for (vtkIdType i = 0; i < values->GetNumberOfValues(); ++i)
            indices_.emplace(values->GetValue(i), i);
    }
    vtkIdType Find(double value) {
        if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min()
            || value > std::numeric_limits<std::int32_t>::max() || std::trunc(value) != value)
            return -1;
        const auto found = indices_.find(static_cast<std::int32_t>(value));
        const auto size = this->GetNumberOfTableValues();
        return found == indices_.end() || size <= 0 ? -1 : found->second % size;
    }
    template<class Value>
    void MapRows(const Value* input, unsigned char* output, int count, int increment, int format) {
        unsigned char nan[4];
        this->GetColorAsUnsignedChars(this->GetNanColor(), nan);
        const auto alpha = this->GetAlpha();
        for (int i = 0; i < count; ++i, input += increment, output += format) {
            const auto index = Find(static_cast<double>(*input));
            const auto* color = index < 0 ? nan : this->GetPointer(index);
            if (format == VTK_RGB || format == VTK_RGBA) {
                for (int c = 0; c < 3; ++c) output[c] = color[c];
                if (format == VTK_RGBA)
                    output[3] = static_cast<unsigned char>(color[3] * alpha + 0.5);
            } else {
                output[0] = static_cast<unsigned char>(color[0] * .30 + color[1] * .59 + color[2] * .11 + .5);
                if (format == VTK_LUMINANCE_ALPHA)
                    output[1] = static_cast<unsigned char>(color[3] * alpha + .5);
            }
        }
    }
    std::unordered_map<std::int32_t, vtkIdType> indices_;
    bool hasIndex_ = false;
};

inline vtkStandardNewMacro(GapLabelLookupTable);
