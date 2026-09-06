#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace eclipse {

struct StreamStatisticsSnapshot {
    double totalFps = 0;
    double receivedFps = 0;
    double decodedFps = 0;
    double presentedFps = 0;
    double bitrateMbps = 0;
    double frameLossPercent = 0;
    double jitterLossPercent = 0;
    double minimumHostLatencyMs = 0;
    double maximumHostLatencyMs = 0;
    double averageHostLatencyMs = 0;
    double averageReassemblyMs = 0;
    double averageDecodeMs = 0;
    double averagePresentMs = 0;
    double averageQueueDelayMs = 0;
    bool hasHostLatency = false;
    std::uint64_t queueDrops = 0;
    std::uint32_t rttMs = 0;
    std::uint32_t rttVarianceMs = 0;
};

struct StreamStatisticsSample {
    std::uint64_t sequence = 0;
    std::uint64_t elapsedMs = 0;
    StreamStatisticsSnapshot statistics;
};

struct OverlayBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

class StreamStatistics {
public:
    using Clock = std::chrono::steady_clock;

    explicit StreamStatistics(Clock::time_point startedAt = Clock::now());

    void recordVideoUnit(int frameNumber, int bytes, std::uint16_t hostLatencyTenthsMs,
                         std::uint64_t reassemblyUs) noexcept;
    void recordDecoded(std::uint64_t durationUs) noexcept;
    void recordPresented(std::uint64_t durationUs, std::uint64_t queueDelayUs = 0) noexcept;
    void recordQueueDrop() noexcept;
    void recordRtt(std::uint32_t rttMs, std::uint32_t varianceMs) noexcept;
    [[nodiscard]] std::optional<StreamStatisticsSample> sampleIfDue(
        Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] std::optional<StreamStatisticsSample> latest() const noexcept;

private:
    struct Counters {
        std::uint64_t received = 0;
        std::uint64_t decoded = 0;
        std::uint64_t presented = 0;
        std::uint64_t bytes = 0;
        std::uint64_t missing = 0;
        std::uint64_t hostLatencyTenthsMs = 0;
        std::uint64_t hostLatencySamples = 0;
        std::uint64_t reassemblyUs = 0;
        std::uint64_t decodeUs = 0;
        std::uint64_t presentUs = 0;
        std::uint64_t queueDelayUs = 0;
        std::uint64_t queueDrops = 0;
    };

    [[nodiscard]] StreamStatisticsSnapshot sampleLocked(Clock::time_point now) noexcept;

    mutable std::mutex mutex_;
    Counters counters_;
    Counters previous_;
    Clock::time_point startedAt_;
    Clock::time_point previousSample_;
    Clock::time_point nextSample_;
    std::uint64_t sequence_ = 0;
    std::optional<StreamStatisticsSample> latest_;
    int previousFrameNumber_ = -1;
    std::uint16_t minimumHostLatencyTenthsMs_ = 0;
    std::uint16_t maximumHostLatencyTenthsMs_ = 0;
    std::uint32_t rttMs_ = 0;
    std::uint32_t rttVarianceMs_ = 0;
};

[[nodiscard]] std::string formatStreamStatistics(const StreamStatisticsSnapshot& statistics,
                                                 int width, int height,
                                                  const std::string& codec);
[[nodiscard]] OverlayBitmap rasterizePerformanceOverlay(const std::string& text, int scale = 2);

}  // namespace eclipse
