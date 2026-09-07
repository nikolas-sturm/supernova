#pragma once

namespace terra {

enum class DisplayMode {
    fullscreen,
    borderless,
    windowed,
};

enum class VideoCodec {
    automatic,
    h264,
    hevc,
    av1,
};

enum class AudioConfig {
    stereo,
    surround51,
    surround71,
};

enum class SystemKeyCapture {
    off,
    fullscreen,
    always,
};

constexpr int terraAudioConfiguration(AudioConfig config) noexcept {
    switch (config) {
        case AudioConfig::surround51:
            return (0x3F << 16) | (6 << 8) | 0xCA;
        case AudioConfig::surround71:
            return (0x63F << 16) | (8 << 8) | 0xCA;
        case AudioConfig::stereo:
        default:
            return (0x3 << 16) | (2 << 8) | 0xCA;
    }
}

constexpr int surroundAudioInfo(AudioConfig config) noexcept {
    const auto audioConfiguration = terraAudioConfiguration(config);
    return (audioConfiguration & 0xFFFF0000) | ((audioConfiguration >> 8) & 0xFF);
}

constexpr int audioChannelCount(AudioConfig config) noexcept {
    switch (config) {
        case AudioConfig::surround51: return 6;
        case AudioConfig::surround71: return 8;
        case AudioConfig::stereo:
        default: return 2;
    }
}

struct InputSettings {
    bool absoluteMouseMode = false;
    SystemKeyCapture captureSystemKeys = SystemKeyCapture::off;
    bool fullscreen = false;
    bool touchscreenTrackpad = true;
    bool swapMouseButtons = false;
    bool reverseScrollDirection = false;
    bool swapFaceButtons = false;
    bool forceGamepad = false;
    bool backgroundGamepad = false;
};

struct StreamSettings {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrateKbps = 10000;
    DisplayMode displayMode = DisplayMode::windowed;
    int displayIndex = 0;
    bool enableVsync = true;
    bool muteHostAudio = true;
    bool gameOptimizations = true;
    bool quitAppAfter = false;
    bool connectionWarnings = true;
    bool detectBlockedConnections = false;
    bool showPerformanceStats = false;
    bool keepAwake = true;
    AudioConfig audioConfig = AudioConfig::stereo;
    VideoCodec videoCodec = VideoCodec::automatic;
    bool enableHdr = false;
    bool enableYuv444 = false;
    InputSettings input;
};

}  // namespace terra
