#pragma once

#include <string>
#include <vector>

namespace terra {

/** @brief One mode supported by a local client display output. */
struct ClientDisplayMode {
    int width = 0;
    int height = 0;
    int refreshRate = 0;
};

/**
 * @brief One physical display attached to the Terra client.
 *
 * Identifiers are host-local, stable strings supplied by the platform so that
 * per-output preferences survive restarts without depending on enumeration
 * order. They are never sent to the streaming host.
 */
struct ClientDisplayOutput {
    std::string id;
    std::string name;
    bool primary = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int refreshRate = 0;
    std::vector<ClientDisplayMode> modes;
};

/**
 * @brief Enumerate the client's currently attached display outputs.
 *
 * Returns an empty list when the platform's display API is unavailable, for
 * example in a headless session. The caller treats an empty list as "no
 * client topology" and falls back to the single configured display.
 */
[[nodiscard]] std::vector<ClientDisplayOutput> enumerateClientDisplays();

}  // namespace terra
