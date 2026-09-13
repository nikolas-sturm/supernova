#pragma once

#include <utility>
#include <vector>

namespace terra {

enum class WorkspacePointerEventType { position, button, cancel };

struct WorkspacePointerEvent {
    WorkspacePointerEventType type;
    int x = 0;
    int y = 0;
    unsigned char button = 0;
    bool pressed = false;
};

// Motion cannot accumulate between button/cancel barriers. Button traffic is a
// small, trusted local source; there is no bounded queue that could lose releases.
class WorkspacePointerState {
public:
    void enter(int x, int y) {
        inside_ = true;
        motion(x, y);
    }

    void motion(int x, int y) {
        if (!inside_) return;
        x_ = x;
        y_ = y;
        const WorkspacePointerEvent event{WorkspacePointerEventType::position, x, y};
        if (!events_.empty() && events_.back().type == WorkspacePointerEventType::position) {
            events_.back() = event;
        } else {
            events_.push_back(event);
        }
    }

    void button(unsigned char button, bool pressed) {
        if (!inside_) return;
        events_.push_back({WorkspacePointerEventType::button, x_, y_, button, pressed});
    }

    void leave() {
        if (!inside_) return;
        inside_ = false;
        events_.push_back({WorkspacePointerEventType::cancel});
    }

    std::vector<WorkspacePointerEvent> takeEvents() {
        return std::exchange(events_, {});
    }

    void clear() {
        events_.clear();
        inside_ = false;
        x_ = 0;
        y_ = 0;
    }

private:
    std::vector<WorkspacePointerEvent> events_;
    bool inside_ = false;
    int x_ = 0;
    int y_ = 0;
};

}  // namespace terra
