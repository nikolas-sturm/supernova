#pragma once

#include "workspace_mouse.h"

#include <optional>
#include <vector>

struct SDL_Window;

namespace terra {

// Call on the SDL render thread after the first buffer is actually presented,
// with the window already SDL fullscreen. Throws on discovery/request failure;
// success does not prove placement. Validate later using compositor-fed geometry.
void requestWaylandFullscreenOutput(SDL_Window* window, const MouseRectangle& assigned);

// Maps SDL display-space workspace rectangles onto xdg-output logical
// rectangles for pointer input. Returns nullopt when correlation is not
// unique; throws only on Wayland protocol failures.
std::optional<std::vector<WorkspaceMouseDisplay>> waylandLogicalWorkspace(
    SDL_Window* window, const std::vector<WorkspaceMouseDisplay>& map);

} // namespace terra
