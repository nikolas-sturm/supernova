#include "workspace_mouse.h"
#include "../../../sol/src/input_workspace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void roundTrip(const std::vector<terra::WorkspaceMouseDisplay>& displays,
               int x, int y, float expectedX, float expectedY) {
    const auto packet = terra::workspaceMousePosition(displays, x, y);
    expect(packet.has_value(), "Client rejected a workspace coordinate.");
    std::vector<input::mouse_viewport_t> host;
    for (const auto& display : displays) {
        const auto& remote = display.remote;
        host.push_back({remote.x, remote.y, remote.width, remote.height});
    }
    // Match the pinned Moonlight InputStream.c wire encoding, not its API arguments.
    const auto position = input::workspace_mouse_position(host, packet->x, packet->y,
                                                         packet->width - 1, packet->height - 1);
    expect(position.has_value(), "Host rejected a mapped workspace coordinate.");
    expect(std::abs(position->first - expectedX) <= 1 && std::abs(position->second - expectedY) <= 1,
           "Workspace round trip moved the pointer to the wrong pixel.");
}
}  // namespace

int main() {
    const terra::MouseRectangle assigned{-1920, 0, 1920, 1080};
    std::vector<terra::MouseRectangle> outputs{{0, 0, 2560, 1440}, assigned};
    expect(terra::workspaceDisplayIndex(outputs, assigned) == 1, "Assigned output was not resolved.");
    std::reverse(outputs.begin(), outputs.end());
    expect(terra::workspaceDisplayIndex(outputs, assigned) == 0,
           "Worker reused an output index from a different enumeration order.");
    expect(!terra::workspaceDisplayIndex(outputs, {3000, 0, 1920, 1080}),
           "Missing assigned output silently fell back to another monitor.");
    outputs.push_back(assigned);
    expect(!terra::workspaceDisplayIndex(outputs, assigned), "Ambiguous output bounds were accepted.");
    expect(terra::workspaceWindowMatches(assigned, assigned, 1920, 1080), "Matching placement was rejected.");
    expect(!terra::workspaceWindowMatches(assigned, {0, 0, 1920, 1080}, 1920, 1080),
           "A same-size window on the wrong monitor enabled mapped input.");
    expect(!terra::workspaceWindowMatches(assigned, assigned, 1280, 720),
           "An unsettled window size enabled mapped input.");

    // Enumeration interleaves an embedded panel between two externals; the
    // selection must claim physically adjacent outputs by desktop origin.
    const std::vector<terra::MouseRectangle> mixedScale{
        {1440, 0, 2560, 1440},   // selected external, enumeration 0
        {0, 270, 2880, 1800},    // embedded panel, enumeration 1
        {4000, 0, 2560, 1440},   // second external, enumeration 2
    };
    expect((terra::workspaceOutputOrder(mixedScale, 0) == std::vector<std::size_t>{0, 2, 1}),
           "Output order included the embedded panel between adjacent externals.");
    expect((terra::workspaceOutputOrder(mixedScale, 2) == std::vector<std::size_t>{2, 1, 0}),
           "Output order did not wrap from the rightmost output.");
    expect((terra::workspaceOutputOrder(mixedScale, 1) == std::vector<std::size_t>{1, 0, 2}),
           "Panel selection did not proceed to the next output by position.");
    expect((terra::workspaceOutputOrder(mixedScale, 9) == std::vector<std::size_t>{1, 0, 2}),
           "Out-of-range selection did not fall back to leftmost position order.");
    expect(terra::workspaceOutputOrder({}, 0).empty(), "Empty enumeration produced an output order.");

    expect(terra::workspaceMouseBlocker(true, 2, 2, true, true) == nullptr,
           "Eligible workspace mouse routing was disabled.");
    expect(terra::workspaceMouseBlocker(true, 3, 3, true, true) == nullptr,
           "Three-output workspace mouse routing was disabled.");
    const auto explained = [](const char* reason, std::string_view expected) {
        expect(reason && std::string_view{reason}.find(expected) != std::string_view::npos,
               "Workspace routing blocker is missing or incorrect.");
    };
    explained(terra::workspaceMouseBlocker(false, 2, 2, true, true), "Windowed");
    explained(terra::workspaceMouseBlocker(true, 3, 2, true, true), "local outputs");
    explained(terra::workspaceMouseBlocker(true, 2, 2, false, true), "scaling or rotation");
    explained(terra::workspaceMouseBlocker(true, 2, 2, true, false), "workspace-mouse-v1");
    expect(terra::workspaceMouseBlocker(true, 1, 2, true, true) != nullptr &&
               terra::workspaceMouseBlocker(true, 5, 5, true, true) != nullptr,
           "Unsupported stream count enabled workspace mouse routing.");

    std::vector<terra::WorkspaceMouseDisplay> displays{
        {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
        {{1920, 0, 2560, 1440}, {1920, 0, 3840, 2160}},
    };
    roundTrip(displays, 100, 200, 100, 200);
    roundTrip(displays, 1919, 500, 1919, 500);
    roundTrip(displays, 1920, 500, 1920, 750);
    roundTrip(displays, 2500, 500, 2790, 750);
    roundTrip(displays, 4479, 1439, 5758.5F, 2158.5F);
    std::reverse(displays.begin(), displays.end());
    roundTrip(displays, 100, 200, -1820, 200);
    roundTrip(displays, 1919, 500, -1, 500);
    roundTrip(displays, 1920, 500, 0, 750);

    displays = {
        {{0, 0, 1920, 1080}, {0, 0, 1920, 1080}},
        {{-1280, 0, 1280, 1024}, {-1920, 0, 1920, 1080}},
        {{0, -1080, 1920, 1080}, {0, -2160, 3840, 2160}},
        {{1920, 0, 3840, 2160}, {1920, 0, 3840, 2160}},
    };
    roundTrip(displays, -1280, 152, -1920, 0);
    roundTrip(displays, -640, 512, -960, 540);
    roundTrip(displays, -1, 1023, -1.5F, 1079);
    roundTrip(displays, 960, -540, 1920, -1080);
    roundTrip(displays, 5759, 2159, 5759, 2159);
    expect(!terra::workspaceMousePosition(displays, -1281, 100), "Outside desktop was mapped.");
    expect(!terra::workspaceMousePosition(displays, -100, -100), "Desktop gap was mapped.");
    const auto good = displays;
    displays[1].local = displays[0].local;
    expect(!terra::workspaceMousePosition(displays, 0, 0), "Ambiguous local outputs were accepted.");
    displays = good;
    displays[0].remote.width = 0;
    expect(!terra::workspaceMousePosition(displays, 0, 0), "Zero-sized source was accepted.");
    expect(!terra::workspaceMousePosition({}, 0, 0), "Empty topology was accepted.");

    const std::vector<input::mouse_viewport_t> host{{0, 0, 1920, 1080}, {-1920, 100, 1920, 1080}};
    expect(!input::workspace_mouse_position(host, -10, 0, 1920, 1080), "Host accepted a workspace gap.");
    expect(!input::workspace_mouse_position(host, 1920, 0, 1920, 1080), "Host accepted an unbound display.");
    expect(!input::workspace_mouse_position(host, 0, 0, 0, 1080), "Host accepted zero reference width.");
    expect(!input::workspace_mouse_position(host, 0, 0, -1, 1080), "Host accepted negative reference width.");
    expect(!input::workspace_mouse_position(host, std::numeric_limits<float>::infinity(), 0, 1920, 1080),
           "Host accepted nonfinite coordinates.");
    expect(!input::workspace_mouse_position({}, 0, 0, 1920, 1080), "Unnegotiated workspace was accepted.");
}
