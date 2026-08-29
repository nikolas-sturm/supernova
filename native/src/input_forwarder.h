#pragma once

#include <cstdint>

#include "stream_settings.h"

namespace eclipse {

std::uint16_t connectedGamepadMask();

}  // namespace eclipse

#ifdef _WIN32

#include <windows.h>

#include <memory>

namespace eclipse {

class InputForwarder {
 public:
    InputForwarder();
    ~InputForwarder();

    InputForwarder(const InputForwarder&) = delete;
    InputForwarder& operator=(const InputForwarder&) = delete;

    void start(HWND window, InputSettings settings, int width, int height, int fps);
    void stop();
    bool handleMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eclipse

#endif
