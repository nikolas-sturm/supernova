#pragma once

#include <istream>
#include <optional>
#include <ostream>

#include <nlohmann/json.hpp>

#include "stream_session.h"

namespace terra {

/** @brief Serialize worker configuration for same-version child process. */
[[nodiscard]] nlohmann::json
streamWorkerConfigJson(const StreamSessionConfig &config);

/** @brief Parse validated same-version worker configuration. */
[[nodiscard]] StreamSessionConfig
parseStreamWorkerConfig(const nlohmann::json &value);

/** @brief Write one length-prefixed JSON worker message. */
bool writeStreamWorkerFrame(std::ostream &output, const nlohmann::json &value);

/** @brief Read one bounded length-prefixed JSON worker message. */
[[nodiscard]] std::optional<nlohmann::json>
readStreamWorkerFrame(std::istream &input);

/** @brief Run worker protocol on standard input and output. */
int runStreamWorker();

} // namespace terra
