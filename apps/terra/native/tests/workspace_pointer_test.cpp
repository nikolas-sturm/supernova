#include "workspace_pointer_state.h"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void drag(int outsideX) {
    terra::WorkspacePointerState state;
    state.enter(100, 200);
    state.button(1, true);
    // The implicit pointer grab keeps source ownership outside its dimensions.
    state.motion(outsideX, -25);
    state.button(1, false);
    state.leave();
    const auto events = state.takeEvents();
    using enum terra::WorkspacePointerEventType;
    expect(events.size() == 5, "Drag ordering lost an event.");
    expect(events[0].type == position && events[0].x == 100 && events[0].y == 200,
           "Enter position missing.");
    expect(events[1].type == button && events[1].button == 1 && events[1].pressed &&
               events[1].x == 100 && events[1].y == 200, "Press position incorrect.");
    expect(events[2].type == position && events[2].x == outsideX && events[2].y == -25,
           "Outside motion was clamped or suppressed.");
    expect(events[3].type == button && events[3].button == 1 && !events[3].pressed &&
               events[3].x == outsideX && events[3].y == -25, "Outside release lost.");
    expect(events[4].type == cancel, "Leave must follow release.");
    expect(state.takeEvents().empty(), "Events were not drained.");
}
}  // namespace

int main() {
    // Source-left and source-right layouts, respectively; no dimensions/clamping.
    drag(2500);
    drag(-700);

    using enum terra::WorkspacePointerEventType;
    terra::WorkspacePointerState state;
    state.motion(1, 2);
    state.button(1, true);
    state.leave();
    expect(state.takeEvents().empty(), "Input before enter was accepted.");

    state.enter(-1, -2);
    for (int i = 0; i < 10000; ++i) state.motion(i, -i);
    state.button(3, true);
    state.motion(-3000, 4000);
    state.motion(-3001, 4001);
    state.button(3, false);
    state.motion(7, 8);
    state.leave();
    state.motion(99, 99);
    state.button(2, true);
    state.button(2, false);
    state.leave();
    state.enter(-50, 6000);
    state.motion(-51, 6001);
    const auto events = state.takeEvents();
    expect(events.size() == 7, "Coalescing crossed a barrier or accepted post-leave input.");
    expect(events[0].type == position && events[0].x == 9999 && events[0].y == -9999,
           "Contiguous motion was not coalesced.");
    expect(events[1].type == button && events[1].button == 3 && events[1].pressed,
           "Press barrier lost.");
    expect(events[2].type == position && events[2].x == -3001 && events[2].y == 4001,
           "Motion crossed a button barrier.");
    expect(events[3].type == button && !events[3].pressed && events[3].x == -3001 &&
               events[3].y == 4001, "Release did not use latest coordinates.");
    expect(events[4].type == position && events[4].x == 7 && events[4].y == 8,
           "Pre-cancel position lost.");
    expect(events[5].type == cancel, "Cancel barrier lost.");
    expect(events[6].type == position && events[6].x == -51 && events[6].y == 6001,
           "Re-enter did not restore pointer input.");

    state.button(1, true);
    state.leave();
    const auto cancelled = state.takeEvents();
    expect(cancelled.size() == 2 && cancelled[0].type == button && cancelled[0].pressed &&
               cancelled[1].type == cancel, "Cancellation did not follow an outstanding press.");

    state.enter(50, 60);
    state.button(5, true);
    state.clear();
    state.clear();
    state.motion(-10, 20);
    state.button(5, false);
    state.leave();
    expect(state.takeEvents().empty(), "Teardown retained events or pointer ownership.");
    state.enter(-10, 20);
    state.button(4, true);
    const auto restarted = state.takeEvents();
    expect(restarted.size() == 2 && restarted[0].type == position &&
               restarted[1].type == button && restarted[1].x == -10 && restarted[1].y == 20 &&
               restarted[1].button == 4, "Restart retained old coordinates or focus.");
}
