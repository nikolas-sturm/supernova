#include "../src/sdl_fullscreen.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Call {
    char api;
    SDL_Window* window;
    Uint32 flags = 0;
    int x = 0, y = 0;
    bool operator==(const Call&) const = default;
};
std::vector<Call> calls;
char token;
SDL_Window* const window = reinterpret_cast<SDL_Window*>(&token);
int exitResult, enterResult, selectedDisplay;
bool mismatch, fullscreen;
std::string error;

void require(bool condition) {
    if (!condition) std::abort();
}
void reset() {
    calls.clear();
    error.clear();
    exitResult = enterResult = 0;
    selectedDisplay = 1; // Stale center selection, not the requested display.
    mismatch = false;
    fullscreen = true; // Must leave fullscreen before retargeting.
}
} // namespace

extern "C" int SDLCALL SDL_SetWindowFullscreen(SDL_Window* w, Uint32 flags) {
    calls.push_back({'F', w, flags});
    const int result = flags == 0 ? exitResult : enterResult;
    if (result == 0) fullscreen = flags != 0;
    if (flags != 0) require(selectedDisplay == 2);
    return result;
}
extern "C" void SDLCALL SDL_SetWindowPosition(SDL_Window* w, int x, int y) {
    calls.push_back({'P', w, 0, x, y});
    require(!fullscreen);
    if (!mismatch) selectedDisplay = x & 0xffff;
}
extern "C" int SDLCALL SDL_GetWindowDisplayIndex(SDL_Window* w) {
    calls.push_back({'D', w});
    return selectedDisplay;
}
extern "C" int SDLCALL SDL_SetError(const char* format, ...) {
    calls.push_back({'E', nullptr});
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    error = buffer;
    return -1;
}

int main() {
    // Public API sequencing only: no video initialization or compositor proof.
    const int center = SDL_WINDOWPOS_CENTERED_DISPLAY(2);
    const std::vector<Call> positioned = {
        {'F', window, 0}, {'P', window, 0, center, center}, {'D', window}};
    for (Uint32 flags : {Uint32(SDL_WINDOW_FULLSCREEN_DESKTOP), Uint32(SDL_WINDOW_FULLSCREEN)}) {
        reset();
        require(terra::setFullscreenOnDisplay(window, 2, flags) == 0);
        auto expected = positioned;
        expected.push_back({'F', window, flags});
        require(calls == expected && selectedDisplay == 2 && fullscreen && error.empty());

        reset();
        enterResult = -7;
        require(terra::setFullscreenOnDisplay(window, 2, flags) == -7);
        require(calls == expected && !fullscreen && error.empty());
    }
    reset();
    exitResult = -3;
    require(terra::setFullscreenOnDisplay(window, 2, SDL_WINDOW_FULLSCREEN) == -3);
    require(calls == std::vector<Call>{{'F', window, 0}} && fullscreen && error.empty());

    reset();
    mismatch = true;
    require(terra::setFullscreenOnDisplay(window, 2, SDL_WINDOW_FULLSCREEN) == -1);
    auto expected = positioned;
    expected.push_back({'E', nullptr});
    require(calls == expected && !fullscreen);
    require(error == "Cannot enter fullscreen on display 2: SDL selected display 1; "
                     "check display availability and retry positioning.");
}
