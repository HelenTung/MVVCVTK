#pragma once

class vtkCommand {
public:
    static constexpr unsigned long KeyPressEvent = 1;
    static constexpr unsigned long KeyReleaseEvent = 2;
    static constexpr unsigned long CharEvent = 3;
};
