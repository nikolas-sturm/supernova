#pragma once

#include "workspace_mouse.h"

struct SDL_Window;

namespace terra {

// Call on the SDL render thread after the first buffer is actually presented,
// with the window already SDL fullscreen. Throws on discovery/request failure;
// success does not prove placement. Validate later using compositor-fed geometry.
void requestWaylandFullscreenOutput(SDL_Window* window, const MouseRectangle& assigned);

} // namespace terra
