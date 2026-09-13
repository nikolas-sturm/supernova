#pragma once

#include <SDL.h>

namespace terra {
inline int setFullscreenOnDisplay(SDL_Window* window, int displayIndex, Uint32 flags) {
    const int result = SDL_SetWindowFullscreen(window, 0);
    if (result < 0) return result;

    // Wayland ignores native positioning, but SDL uses cached windowed geometry
    // to target the fullscreen output. Caller shows first; do not pump/show here.
    const int position = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
    SDL_SetWindowPosition(window, position, position);
    const int actualDisplay = SDL_GetWindowDisplayIndex(window);
    if (actualDisplay != displayIndex) {
        SDL_SetError("Cannot enter fullscreen on display %d: SDL selected display %d; "
                     "check display availability and retry positioning.", displayIndex, actualDisplay);
        return -1;
    }
    return SDL_SetWindowFullscreen(window, flags);
}
} // namespace terra
