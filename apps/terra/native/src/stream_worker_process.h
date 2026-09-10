#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include "stream_session.h"

namespace terra {

/** @brief Same-binary child process isolating one Moonlight transport. */
class StreamWorkerProcess {
public:
  using Listener = std::function<void(const nlohmann::json &)>;

  explicit StreamWorkerProcess(Listener listener);
  ~StreamWorkerProcess();
  StreamWorkerProcess(const StreamWorkerProcess &) = delete;
  StreamWorkerProcess &operator=(const StreamWorkerProcess &) = delete;

  /** @brief Spawn child and send pipe-only stream configuration. */
  void start(const StreamSessionConfig &config);
  /** @brief Wait for child's Moonlight connection callback. */
  [[nodiscard]] bool waitConnected(std::chrono::milliseconds timeout);
  /** @brief Request child stop and reap process. */
  void stop() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace terra
