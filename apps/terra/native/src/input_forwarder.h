#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "stream_settings.h"

namespace terra {

std::uint16_t connectedGamepadMask();
bool gamepadTransportAvailable() noexcept;
[[nodiscard]] std::optional<std::string> prepareClipboardText(std::string_view text);

}  // namespace terra

#if defined(_WIN32)

#include <windows.h>

#include <memory>

namespace terra {

class InputForwarder {
 public:
    InputForwarder();
    ~InputForwarder();

    InputForwarder(const InputForwarder&) = delete;
    InputForwarder& operator=(const InputForwarder&) = delete;

    void start(HWND window, InputSettings settings, int width, int height, int fps,
               std::function<void()> toggleStatistics = {},
               std::function<bool()> toggleFullscreen = {},
               std::function<void()> openOverlay = {});
    void resumeAfterOverlay();
    void setEnabled(bool enabled);
    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                          std::uint16_t highFrequency);
    void setGamepadTriggerRumble(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                 std::uint16_t rightTrigger);
    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz);
    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue);
    void updateGamepads();
    void stop();
    bool handleMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terra

#elif defined(__linux__) && defined(TERRA_HAS_LINUX_VIDEO)

#include <memory>

#include <SDL.h>

namespace terra {

class InputForwarder {
 public:
    InputForwarder();
    ~InputForwarder();

    InputForwarder(const InputForwarder&) = delete;
    InputForwarder& operator=(const InputForwarder&) = delete;

    void start(SDL_Window* window, InputSettings settings, int width, int height, int fps,
               std::function<void()> toggleStatistics = {},
               std::function<bool()> toggleFullscreen = {},
               std::function<void()> openOverlay = {});
    void resumeAfterOverlay();
    void setEnabled(bool enabled);
    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                          std::uint16_t highFrequency);
    void setGamepadTriggerRumble(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                 std::uint16_t rightTrigger);
    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz);
    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue);
    void updateGamepads();
    void stop();
    void handleEvent(const SDL_Event& event);

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terra

#endif
