#include "stream_worker_protocol.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

#include "stream_statistics.h"

namespace terra {
namespace {
constexpr std::uint32_t kMaxFrameBytes = 1024 * 1024;

nlohmann::json settingsJson(const StreamSettings &settings) {
  auto workspaceMouse = nlohmann::json::array();
  for (const auto &display : settings.input.workspaceMouse) {
    const auto rectangle = [](const MouseRectangle &rect) {
      return nlohmann::json::array({rect.x, rect.y, rect.width, rect.height});
    };
    workspaceMouse.push_back({{"local", rectangle(display.local)},
                              {"remote", rectangle(display.remote)}});
  }
  return {
      {"width", settings.width},
      {"height", settings.height},
      {"fps", settings.fps},
      {"bitrateKbps", settings.bitrateKbps},
      {"displayMode", static_cast<int>(settings.displayMode)},
      {"displayIndex", settings.displayIndex},
      {"enableVsync", settings.enableVsync},
      {"muteHostAudio", settings.muteHostAudio},
      {"gameOptimizations", settings.gameOptimizations},
      {"quitAppAfter", settings.quitAppAfter},
      {"connectionWarnings", settings.connectionWarnings},
      {"detectBlockedConnections", settings.detectBlockedConnections},
      {"showPerformanceStats", settings.showPerformanceStats},
      {"keepAwake", settings.keepAwake},
      {"audioConfig", static_cast<int>(settings.audioConfig)},
      {"videoCodec", static_cast<int>(settings.videoCodec)},
      {"enableHdr", settings.enableHdr},
      {"enableYuv444", settings.enableYuv444},
      {"input",
       {{"absoluteMouseMode", settings.input.absoluteMouseMode},
        {"workspaceMouse", std::move(workspaceMouse)},
        {"captureSystemKeys",
         static_cast<int>(settings.input.captureSystemKeys)},
        {"fullscreen", settings.input.fullscreen},
        {"touchscreenTrackpad", settings.input.touchscreenTrackpad},
        {"swapMouseButtons", settings.input.swapMouseButtons},
        {"reverseScrollDirection", settings.input.reverseScrollDirection},
        {"swapFaceButtons", settings.input.swapFaceButtons},
        {"forceGamepad", settings.input.forceGamepad},
        {"backgroundGamepad", settings.input.backgroundGamepad}}},
  };
}

StreamSettings parseSettings(const nlohmann::json &value) {
  StreamSettings settings;
  settings.width = value.at("width").get<int>();
  settings.height = value.at("height").get<int>();
  settings.fps = value.at("fps").get<int>();
  settings.bitrateKbps = value.at("bitrateKbps").get<int>();
  settings.displayMode =
      static_cast<DisplayMode>(value.at("displayMode").get<int>());
  settings.displayIndex = value.at("displayIndex").get<int>();
  settings.enableVsync = value.at("enableVsync").get<bool>();
  settings.muteHostAudio = value.at("muteHostAudio").get<bool>();
  settings.gameOptimizations = value.at("gameOptimizations").get<bool>();
  settings.quitAppAfter = value.at("quitAppAfter").get<bool>();
  settings.connectionWarnings = value.at("connectionWarnings").get<bool>();
  settings.detectBlockedConnections =
      value.at("detectBlockedConnections").get<bool>();
  settings.showPerformanceStats = value.at("showPerformanceStats").get<bool>();
  settings.keepAwake = value.at("keepAwake").get<bool>();
  settings.audioConfig =
      static_cast<AudioConfig>(value.at("audioConfig").get<int>());
  settings.videoCodec =
      static_cast<VideoCodec>(value.at("videoCodec").get<int>());
  settings.enableHdr = value.at("enableHdr").get<bool>();
  settings.enableYuv444 = value.at("enableYuv444").get<bool>();
  const auto &input = value.at("input");
  settings.input.absoluteMouseMode = input.at("absoluteMouseMode").get<bool>();
  for (const auto &display : input.at("workspaceMouse")) {
    const auto rectangle = [](const nlohmann::json &rect) {
      const auto values = rect.get<std::array<int, 4>>();
      return MouseRectangle{values[0], values[1], values[2], values[3]};
    };
    settings.input.workspaceMouse.push_back(
        {rectangle(display.at("local")), rectangle(display.at("remote"))});
  }
  settings.input.captureSystemKeys =
      static_cast<SystemKeyCapture>(input.at("captureSystemKeys").get<int>());
  settings.input.fullscreen = input.at("fullscreen").get<bool>();
  settings.input.touchscreenTrackpad =
      input.at("touchscreenTrackpad").get<bool>();
  settings.input.swapMouseButtons = input.at("swapMouseButtons").get<bool>();
  settings.input.reverseScrollDirection =
      input.at("reverseScrollDirection").get<bool>();
  settings.input.swapFaceButtons = input.at("swapFaceButtons").get<bool>();
  settings.input.forceGamepad = input.at("forceGamepad").get<bool>();
  settings.input.backgroundGamepad = input.at("backgroundGamepad").get<bool>();
  return settings;
}
} // namespace

nlohmann::json streamWorkerConfigJson(const StreamSessionConfig &config) {
  return {
      {"type", "start"},
      {"hostId", config.hostId},
      {"appId", config.appId},
      {"appName", config.appName},
      {"address", config.address},
      {"appVersion", config.appVersion},
      {"gfeVersion", config.gfeVersion},
      {"serverCodecModeSupport", config.serverCodecModeSupport},
      {"videoFormat", config.videoFormat},
      {"settings", settingsJson(config.settings)},
      {"audioEnabled", config.audioEnabled},
      {"controllerEnabled", config.controllerEnabled},
      {"launch",
       {{"appId", config.launch.appId},
        {"resumed", config.launch.resumed},
        {"sessionUrl", config.launch.sessionUrl},
        {"remoteInputKey", config.launch.remoteInputKey},
        {"remoteInputIv", config.launch.remoteInputIv},
        {"logicalSessionId", config.launch.logicalSessionId},
        {"childStreamId", config.launch.childStreamId}}},
  };
}

StreamSessionConfig parseStreamWorkerConfig(const nlohmann::json &value) {
  if (!value.is_object() || value.value("type", "") != "start") {
    throw std::invalid_argument("Invalid stream worker start frame.");
  }
  StreamSessionConfig config;
  config.hostId = value.at("hostId").get<std::string>();
  config.appId = value.at("appId").get<int>();
  config.appName = value.at("appName").get<std::string>();
  config.address = value.at("address").get<std::string>();
  config.appVersion = value.at("appVersion").get<std::string>();
  config.gfeVersion = value.at("gfeVersion").get<std::string>();
  config.serverCodecModeSupport = value.at("serverCodecModeSupport").get<int>();
  config.videoFormat = value.at("videoFormat").get<int>();
  config.settings = parseSettings(value.at("settings"));
  config.audioEnabled = value.at("audioEnabled").get<bool>();
  config.controllerEnabled = value.at("controllerEnabled").get<bool>();
  const auto &launch = value.at("launch");
  config.launch.appId = launch.at("appId").get<int>();
  config.launch.resumed = launch.at("resumed").get<bool>();
  config.launch.sessionUrl = launch.at("sessionUrl").get<std::string>();
  config.launch.remoteInputKey =
      launch.at("remoteInputKey").get<std::array<unsigned char, 16>>();
  config.launch.remoteInputIv =
      launch.at("remoteInputIv").get<std::array<unsigned char, 16>>();
  config.launch.logicalSessionId =
      launch.at("logicalSessionId").get<std::string>();
  config.launch.childStreamId = launch.at("childStreamId").get<std::string>();
  return config;
}

bool writeStreamWorkerFrame(std::ostream &output, const nlohmann::json &value) {
  const auto payload = value.dump();
  if (payload.size() > kMaxFrameBytes)
    return false;
  const auto size = static_cast<std::uint32_t>(payload.size());
  const std::array<char, 4> header{
      static_cast<char>(size >> 24U), static_cast<char>(size >> 16U),
      static_cast<char>(size >> 8U), static_cast<char>(size)};
  output.write(header.data(), static_cast<std::streamsize>(header.size()));
  output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  output.flush();
  return output.good();
}

std::optional<nlohmann::json> readStreamWorkerFrame(std::istream &input) {
  std::array<unsigned char, 4> header{};
  if (!input.read(reinterpret_cast<char *>(header.data()), header.size()))
    return std::nullopt;
  const auto size = (static_cast<std::uint32_t>(header[0]) << 24U) |
                    (static_cast<std::uint32_t>(header[1]) << 16U) |
                    (static_cast<std::uint32_t>(header[2]) << 8U) | header[3];
  if (size == 0 || size > kMaxFrameBytes)
    throw std::runtime_error("Invalid worker frame size.");
  std::string payload(size, '\0');
  if (!input.read(payload.data(), static_cast<std::streamsize>(payload.size())))
    return std::nullopt;
  return nlohmann::json::parse(payload);
}

bool workerDisconnectQuitsHost(const nlohmann::json &event,
                               const bool quitAppAfter) {
  return quitAppAfter && event.value("userEnded", false);
}

int runStreamWorker() {
  const auto frame = readStreamWorkerFrame(std::cin);
  if (!frame)
    return 2;
  auto config = parseStreamWorkerConfig(*frame);
  const int displayIndex = config.settings.displayIndex;
  std::fprintf(stderr,
      "[terra-worker] display=%d video=%dx%d fps=%d bitrate_kbps=%d workspace_mouse_displays=%zu\n",
      displayIndex, config.settings.width, config.settings.height, config.settings.fps,
      config.settings.bitrateKbps, config.settings.input.workspaceMouse.size());
  std::mutex outputMutex;
  const auto send = [&](nlohmann::json value) {
    std::scoped_lock lock{outputMutex};
    static_cast<void>(writeStreamWorkerFrame(std::cout, value));
  };
  StreamSession session{
      std::move(config),
      [&](const StreamSessionEvent &event) {
        send({{"type", "status"},
              {"state", event.state},
              {"message", event.message}});
      },
      [&](StreamSessionEvent event, bool hostEnded, bool userEnded) {
        send({{"type", "disconnected"},
              {"state", event.state},
              {"message", event.message},
              {"hostEnded", hostEnded},
              {"userEnded", userEnded}});
      }, {},
      [displayIndex](const StreamStatisticsSample &sample) {
        const auto &s = sample.statistics;
        if (sample.sequence % 5 != 0 && s.frameLossPercent == 0 && s.queueDrops == 0) return;
        // Once per sample at most, never per frame or mouse packet. stderr is separate
        // from the framed worker protocol on stdout.
        std::fprintf(stderr,
            "[terra-stream display=%d t_ms=%llu] received_fps=%.1f presented_fps=%.1f mbps=%.2f loss_pct=%.2f rtt_ms=%u host_avg_ms=%.2f host_max_ms=%.2f reassembly_ms=%.2f decode_ms=%.2f present_ms=%.2f queue_ms=%.2f queue_drops=%llu\n",
            displayIndex, static_cast<unsigned long long>(sample.elapsedMs), s.receivedFps,
            s.presentedFps, s.bitrateMbps, s.frameLossPercent, s.rttMs,
            s.hasHostLatency ? s.averageHostLatencyMs : -1.0,
            s.hasHostLatency ? s.maximumHostLatencyMs : -1.0, s.averageReassemblyMs,
            s.averageDecodeMs, s.averagePresentMs, s.averageQueueDelayMs,
            static_cast<unsigned long long>(s.queueDrops));
      }};
  try {
    session.start();
    while (const auto command = readStreamWorkerFrame(std::cin)) {
      if (command->value("type", "") == "stop")
        break;
    }
    session.requestStop();
    return 0;
  } catch (const std::exception &exception) {
    send({{"type", "error"}, {"message", exception.what()}});
    return 1;
  }
}

} // namespace terra
