#pragma once

#include <functional>
#include <memory>
#include <string>

#include "stream_settings.h"

extern "C" {
#include <Limelight.h>
}

namespace eclipse {

class VideoRenderer {
public:
    using StatusListener = std::function<void(std::string state, std::string message)>;

    VideoRenderer(StreamSettings settings, StatusListener listener);
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    void initialize(int videoFormat, int width, int height, int frameRate);
    [[nodiscard]] int submit(PDECODE_UNIT decodeUnit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eclipse
