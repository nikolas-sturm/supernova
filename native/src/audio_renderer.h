#pragma once

#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <Limelight.h>
}

namespace eclipse {

class AudioRenderer {
public:
    using StatusListener = std::function<void(std::string state, std::string message)>;

    explicit AudioRenderer(StatusListener listener);
    ~AudioRenderer();

    AudioRenderer(const AudioRenderer&) = delete;
    AudioRenderer& operator=(const AudioRenderer&) = delete;

    void initialize(const OPUS_MULTISTREAM_CONFIGURATION& config);
    void submit(const char* data, int length);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eclipse
