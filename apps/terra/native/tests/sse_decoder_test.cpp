#include <cassert>
#include <string>
#include <vector>

#include "sse_decoder.h"

int main() {
    terra::SseDecoder decoder;
    std::vector<std::string> ids;
    std::vector<nlohmann::json> events;
    const auto collect = [&](const std::string& id, const nlohmann::json& event) {
        ids.push_back(id);
        events.push_back(event);
    };

    decoder.push(": keep-alive\r\nid: 41\r\ndata: {\"schema", collect);
    decoder.push("Version\":1,\r\ndata: \"type\":\"session.updated\"}\r\n\r\n", collect);

    assert(ids == std::vector<std::string>{"41"});
    assert(events.size() == 1);
    assert(events.front().at("type") == "session.updated");
    return 0;
}
