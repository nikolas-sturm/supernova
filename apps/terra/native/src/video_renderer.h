#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "stream_settings.h"

extern "C" {
#include <Limelight.h>
}

namespace terra {

class StreamStatistics;

struct StreamWindowBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double scaleFactor = 1.0;
    bool wayland = false;
    bool fullscreen = false;
};

[[nodiscard]] int selectVideoFormat(VideoCodec preference, int serverCodecModeSupport,
                                    bool enableHdr, bool enableYuv444 = false);

class VideoRenderer {
public:
    using StatusListener = std::function<void(std::string state, std::string message)>;
    using CloseListener = std::function<void()>;
    using OverlayListener = std::function<void(const StreamWindowBounds&)>;

    VideoRenderer(StreamSettings settings, StatusListener listener, CloseListener closeListener,
                   std::shared_ptr<StreamStatistics> statistics = {},
                   OverlayListener overlayListener = {});
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    void initialize(int videoFormat, int width, int height, int frameRate);
    void setInputEnabled(bool enabled);
    void setHdrMode(bool enabled);
    void resumeOverlay();
    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                          std::uint16_t highFrequency);
    void setGamepadTriggerRumble(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                 std::uint16_t rightTrigger);
    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz);
    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue);
    [[nodiscard]] int submit(PDECODE_UNIT decodeUnit);
    [[nodiscard]] bool recoveryRequired() const;
    [[nodiscard]] std::optional<int> recoveryDisplayIndex() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terra
