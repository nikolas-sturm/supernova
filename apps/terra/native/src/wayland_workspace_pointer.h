#pragma once

#include "workspace_pointer_state.h"

#include <memory>
#include <optional>

struct SDL_Window;
union SDL_Event;

namespace terra {

// Use only for Wayland multi-display absolute input, on SDL's event-pump thread.
// The window must outlive stop(). SDL owns and dispatches the default queue.
class WaylandWorkspacePointer {
public:
    WaylandWorkspacePointer();
    ~WaylandWorkspacePointer();
    WaylandWorkspacePointer(const WaylandWorkspacePointer&) = delete;
    WaylandWorkspacePointer& operator=(const WaylandWorkspacePointer&) = delete;

    void start(SDL_Window* window);
    void stop();
    // Remains true after seat loss or callback failure: never resume SDL mouse
    // forwarding until stop(). SDL wheel forwarding remains independent.
    bool active() const noexcept;
    std::optional<WorkspacePointerEvent> decodeEvent(const SDL_Event& event) const;
    // Invalidate queued pointer messages without relinquishing compositor pointer focus.
    void discardPendingEvents();
    // Runtime callback failure yields cancel first; subsequent calls rethrow the
    // captured error until stop/start. Startup binding failures throw from start.
    std::vector<WorkspacePointerEvent> takeEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terra
