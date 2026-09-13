#pragma once

#include <SDL.h>

#include "sdl_video_policy.h"

namespace terra {

// Share backend selection between display enumeration and worker startup.
inline int initializeSdlVideo() {
    if (preferNativeWayland(SDL_WasInit(SDL_INIT_VIDEO) != 0,
                            SDL_GetHint("SDL_VIDEODRIVER"),
                            SDL_getenv("XDG_SESSION_TYPE"),
                            SDL_getenv("WAYLAND_DISPLAY"))) {
        // SDL 2.0.16 selects the driver through the environment, not a hint.
        if (SDL_setenv("SDL_VIDEODRIVER", "wayland", 0) != 0) {
            return SDL_SetError("Failed to set SDL_VIDEODRIVER=wayland before SDL video initialization");
        }
    }

    // Always acquire a subsystem reference, even when video is initialized.
    return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

}  // namespace terra
