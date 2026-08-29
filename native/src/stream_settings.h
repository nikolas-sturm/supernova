#pragma once

namespace eclipse {

enum class DisplayMode {
    fullscreen,
    borderless,
    windowed,
};

struct InputSettings {
    bool absoluteMouseMode = false;
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
    bool enableVsync = true;
    bool muteHostAudio = true;
    bool gameOptimizations = true;
    bool connectionWarnings = true;
    bool keepAwake = true;
    InputSettings input;
};

}  // namespace eclipse
