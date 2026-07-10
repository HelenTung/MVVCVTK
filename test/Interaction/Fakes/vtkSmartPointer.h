#pragma once

#include <cstddef>

template <typename T>
class vtkSmartPointer {
public:
    vtkSmartPointer() = default;
    vtkSmartPointer(std::nullptr_t)
    {
    }
    explicit vtkSmartPointer(T* pointer)
        : m_pointer(pointer)
    {
    }

    T* GetPointer() const
    {
        return m_pointer;
    }

    T* operator->() const
    {
        return m_pointer;
    }

    explicit operator bool() const
    {
        return m_pointer != nullptr;
    }

private:
    T* m_pointer = nullptr;
};
