#pragma once

#include <string>
#include <string_view>

namespace terra {

/** @brief Match a session mutation only when it targets this local logical session. */
[[nodiscard]] inline bool
isLocalLogicalSessionMutation(std::string_view localHostId, std::string_view localSessionId,
                              std::string_view hostId, std::string_view method,
                              std::string_view path) {
    if (localSessionId.empty() || localHostId != hostId || method != "POST") return false;
    const auto base = "/eclipse/v1/sessions/" + std::string{localSessionId};
    return path == base + "/disconnect" || path == base + "/stop";
}

}  // namespace terra
