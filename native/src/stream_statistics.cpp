#include "stream_statistics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace eclipse {
namespace {

std::uint64_t difference(std::uint64_t current, std::uint64_t previous) noexcept {
    return current >= previous ? current - previous : current;
}

std::array<std::uint8_t, 7> glyph(char value) noexcept {
    using Rows = std::array<std::uint8_t, 7>;
    switch (value) {
        case 'A': return Rows{14, 17, 17, 31, 17, 17, 17};
        case 'B': return Rows{30, 17, 17, 30, 17, 17, 30};
        case 'C': return Rows{14, 17, 16, 16, 16, 17, 14};
        case 'D': return Rows{30, 17, 17, 17, 17, 17, 30};
        case 'E': return Rows{31, 16, 16, 30, 16, 16, 31};
        case 'F': return Rows{31, 16, 16, 30, 16, 16, 16};
        case 'G': return Rows{14, 17, 16, 23, 17, 17, 14};
        case 'H': return Rows{17, 17, 17, 31, 17, 17, 17};
        case 'I': return Rows{14, 4, 4, 4, 4, 4, 14};
        case 'J': return Rows{7, 2, 2, 2, 18, 18, 12};
        case 'K': return Rows{17, 18, 20, 24, 20, 18, 17};
        case 'L': return Rows{16, 16, 16, 16, 16, 16, 31};
        case 'M': return Rows{17, 27, 21, 21, 17, 17, 17};
        case 'N': return Rows{17, 25, 21, 19, 17, 17, 17};
        case 'O': return Rows{14, 17, 17, 17, 17, 17, 14};
        case 'P': return Rows{30, 17, 17, 30, 16, 16, 16};
        case 'Q': return Rows{14, 17, 17, 17, 21, 18, 13};
        case 'R': return Rows{30, 17, 17, 30, 20, 18, 17};
        case 'S': return Rows{15, 16, 16, 14, 1, 1, 30};
        case 'T': return Rows{31, 4, 4, 4, 4, 4, 4};
        case 'U': return Rows{17, 17, 17, 17, 17, 17, 14};
        case 'V': return Rows{17, 17, 17, 17, 17, 10, 4};
        case 'W': return Rows{17, 17, 17, 21, 21, 21, 10};
        case 'X': return Rows{17, 17, 10, 4, 10, 17, 17};
        case 'Y': return Rows{17, 17, 10, 4, 4, 4, 4};
        case 'Z': return Rows{31, 1, 2, 4, 8, 16, 31};
        case '0': return Rows{14, 17, 19, 21, 25, 17, 14};
        case '1': return Rows{4, 12, 4, 4, 4, 4, 14};
        case '2': return Rows{14, 17, 1, 2, 4, 8, 31};
        case '3': return Rows{30, 1, 1, 14, 1, 1, 30};
        case '4': return Rows{2, 6, 10, 18, 31, 2, 2};
        case '5': return Rows{31, 16, 16, 30, 1, 1, 30};
        case '6': return Rows{14, 16, 16, 30, 17, 17, 14};
        case '7': return Rows{31, 1, 2, 4, 8, 8, 8};
        case '8': return Rows{14, 17, 17, 14, 17, 17, 14};
        case '9': return Rows{14, 17, 17, 15, 1, 1, 14};
        case '.': return Rows{0, 0, 0, 0, 0, 12, 12};
        case ':': return Rows{0, 12, 12, 0, 12, 12, 0};
        case '%': return Rows{17, 2, 4, 8, 17, 0, 0};
        case '+': return Rows{0, 4, 4, 31, 4, 4, 0};
        case '-': return Rows{0, 0, 0, 31, 0, 0, 0};
        case '/': return Rows{1, 2, 4, 8, 16, 0, 0};
        case '|': return Rows{4, 4, 4, 4, 4, 4, 4};
        case '@': return Rows{14, 17, 23, 21, 23, 16, 14};
        case '(': return Rows{2, 4, 8, 8, 8, 4, 2};
        case ')': return Rows{8, 4, 2, 2, 2, 4, 8};
        case ',': return Rows{0, 0, 0, 0, 0, 4, 8};
        default: return Rows{};
    }
}

}  // namespace

StreamStatistics::StreamStatistics(Clock::time_point startedAt)
    : startedAt_(startedAt),
      previousSample_(startedAt),
      nextSample_(startedAt + std::chrono::seconds{1}) {}

void StreamStatistics::recordVideoUnit(int frameNumber, int bytes,
                                       std::uint16_t hostLatencyTenthsMs,
                                       std::uint64_t reassemblyUs) noexcept {
    std::scoped_lock lock{mutex_};
    ++counters_.received;
    counters_.bytes += static_cast<std::uint64_t>(std::max(bytes, 0));
    if (previousFrameNumber_ >= 0 && frameNumber > previousFrameNumber_ + 1) {
        counters_.missing += static_cast<std::uint64_t>(frameNumber - previousFrameNumber_ - 1);
    }
    previousFrameNumber_ = frameNumber;
    if (hostLatencyTenthsMs != 0) {
        counters_.hostLatencyTenthsMs += hostLatencyTenthsMs;
        ++counters_.hostLatencySamples;
        minimumHostLatencyTenthsMs_ = minimumHostLatencyTenthsMs_ == 0
                                          ? hostLatencyTenthsMs
                                          : std::min(minimumHostLatencyTenthsMs_,
                                                     hostLatencyTenthsMs);
        maximumHostLatencyTenthsMs_ =
            std::max(maximumHostLatencyTenthsMs_, hostLatencyTenthsMs);
    }
    counters_.reassemblyUs += reassemblyUs;
}

void StreamStatistics::recordDecoded(std::uint64_t durationUs) noexcept {
    std::scoped_lock lock{mutex_};
    ++counters_.decoded;
    counters_.decodeUs += durationUs;
}

void StreamStatistics::recordPresented(std::uint64_t durationUs,
                                       std::uint64_t queueDelayUs) noexcept {
    std::scoped_lock lock{mutex_};
    ++counters_.presented;
    counters_.presentUs += durationUs;
    counters_.queueDelayUs += queueDelayUs;
}

void StreamStatistics::recordQueueDrop() noexcept {
    std::scoped_lock lock{mutex_};
    ++counters_.queueDrops;
}

void StreamStatistics::recordRtt(std::uint32_t rttMs, std::uint32_t varianceMs) noexcept {
    std::scoped_lock lock{mutex_};
    rttMs_ = rttMs;
    rttVarianceMs_ = varianceMs;
}

StreamStatisticsSnapshot StreamStatistics::sampleLocked(Clock::time_point now) noexcept {
    const auto elapsed = std::chrono::duration<double>(now - previousSample_).count();
    const auto seconds = std::max(elapsed, 0.001);
    const auto received = difference(counters_.received, previous_.received);
    const auto decoded = difference(counters_.decoded, previous_.decoded);
    const auto presented = difference(counters_.presented, previous_.presented);
    const auto missing = difference(counters_.missing, previous_.missing);
    const auto hostSamples = difference(counters_.hostLatencySamples, previous_.hostLatencySamples);
    const auto totalFrames = received + missing;
    StreamStatisticsSnapshot result{
        .totalFps = totalFrames / seconds,
        .receivedFps = received / seconds,
        .decodedFps = decoded / seconds,
        .presentedFps = presented / seconds,
        .bitrateMbps = difference(counters_.bytes, previous_.bytes) * 8.0 / seconds / 1'000'000.0,
        .frameLossPercent = totalFrames == 0 ? 0 : missing * 100.0 / totalFrames,
        .jitterLossPercent = decoded == 0
                                 ? 0
                                 : difference(counters_.queueDrops, previous_.queueDrops) * 100.0 /
                                       decoded,
        .minimumHostLatencyMs = minimumHostLatencyTenthsMs_ / 10.0,
        .maximumHostLatencyMs = maximumHostLatencyTenthsMs_ / 10.0,
        .averageHostLatencyMs = hostSamples == 0
                                    ? 0
                                    : difference(counters_.hostLatencyTenthsMs,
                                                 previous_.hostLatencyTenthsMs) /
                                          static_cast<double>(hostSamples) / 10.0,
        .averageReassemblyMs = received == 0
                                   ? 0
                                   : difference(counters_.reassemblyUs, previous_.reassemblyUs) /
                                         static_cast<double>(received) / 1000.0,
        .averageDecodeMs = decoded == 0
                               ? 0
                               : difference(counters_.decodeUs, previous_.decodeUs) /
                                     static_cast<double>(decoded) / 1000.0,
        .averagePresentMs = presented == 0
                                ? 0
                                : difference(counters_.presentUs, previous_.presentUs) /
                                      static_cast<double>(presented) / 1000.0,
        .averageQueueDelayMs = presented == 0
                                   ? 0
                                   : difference(counters_.queueDelayUs,
                                                previous_.queueDelayUs) /
                                         static_cast<double>(presented) / 1000.0,
        .hasHostLatency = hostSamples != 0,
        .queueDrops = difference(counters_.queueDrops, previous_.queueDrops),
        .rttMs = rttMs_,
        .rttVarianceMs = rttVarianceMs_,
    };
    previous_ = counters_;
    previousSample_ = now;
    minimumHostLatencyTenthsMs_ = 0;
    maximumHostLatencyTenthsMs_ = 0;
    return result;
}

std::optional<StreamStatisticsSample> StreamStatistics::sampleIfDue(Clock::time_point now) noexcept {
    std::scoped_lock lock{mutex_};
    if (now < nextSample_) return std::nullopt;

    latest_ = StreamStatisticsSample{
        .sequence = ++sequence_,
        .elapsedMs = static_cast<std::uint64_t>(
            std::max(std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt_).count(),
                     std::int64_t{0})),
        .statistics = sampleLocked(now),
    };
    nextSample_ = now + std::chrono::seconds{1};
    return latest_;
}

std::optional<StreamStatisticsSample> StreamStatistics::latest() const noexcept {
    std::scoped_lock lock{mutex_};
    return latest_;
}

std::string formatStreamStatistics(const StreamStatisticsSnapshot& statistics, int width,
                                    int height, const std::string& codec) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2);
    output << "Video stream: " << width << "x" << height << " " << statistics.totalFps
           << " FPS (Codec: " << codec << ")\n";
    output << "Incoming frame rate from network: " << statistics.receivedFps << " FPS\n";
    output << "Decoding frame rate: " << statistics.decodedFps << " FPS\n";
    output << "Rendering frame rate: " << statistics.presentedFps << " FPS\n";
    if (statistics.hasHostLatency) {
        output << std::setprecision(1)
               << "Host processing latency min/max/average: "
               << statistics.minimumHostLatencyMs << "/" << statistics.maximumHostLatencyMs
               << "/" << statistics.averageHostLatencyMs << " ms\n"
               << std::setprecision(2);
    }
    output << "Frames dropped by your network connection: " << statistics.frameLossPercent
           << "%\n";
    output << "Frames dropped due to network jitter: " << statistics.jitterLossPercent << "%\n";
    output << "Average network latency: ";
    if (statistics.rttMs == 0) {
        output << "N/A\n";
    } else {
        output << statistics.rttMs << " ms (variance: " << statistics.rttVarianceMs << " ms)\n";
    }
    output << "Average decoding time: " << statistics.averageDecodeMs << " ms\n";
    output << "Average frame queue delay: " << statistics.averageQueueDelayMs << " ms\n";
    output << "Average rendering time (including monitor V-sync latency): "
           << statistics.averagePresentMs << " ms\n";
    return output.str();
}

OverlayBitmap rasterizePerformanceOverlay(const std::string& text, int scale) {
    scale = std::clamp(scale, 1, 4);
    std::size_t columns = 0;
    std::size_t lines = 1;
    std::size_t currentColumns = 0;
    for (const auto character : text) {
        if (character == '\n') {
            columns = std::max(columns, currentColumns);
            currentColumns = 0;
            ++lines;
        } else {
            ++currentColumns;
        }
    }
    columns = std::max(columns, currentColumns);
    constexpr int padding = 6;
    OverlayBitmap bitmap;
    bitmap.width = static_cast<int>(columns) * 6 * scale + padding * 2;
    bitmap.height = static_cast<int>(lines) * 9 * scale + padding * 2;
    bitmap.rgba.resize(static_cast<std::size_t>(bitmap.width) * bitmap.height * 4);
    for (std::size_t pixel = 0; pixel < bitmap.rgba.size(); pixel += 4) {
        bitmap.rgba[pixel] = 4;
        bitmap.rgba[pixel + 1] = 8;
        bitmap.rgba[pixel + 2] = 12;
        bitmap.rgba[pixel + 3] = 190;
    }

    int column = 0;
    int line = 0;
    for (auto character : text) {
        if (character == '\n') {
            column = 0;
            ++line;
            continue;
        }
        if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        const auto rows = glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int bit = 0; bit < 5; ++bit) {
                if ((rows[row] & (1U << (4 - bit))) == 0) continue;
                for (int y = 0; y < scale; ++y) {
                    for (int x = 0; x < scale; ++x) {
                        const int pixelX = padding + (column * 6 + bit) * scale + x;
                        const int pixelY = padding + (line * 9 + row) * scale + y;
                        const auto offset = (static_cast<std::size_t>(pixelY) * bitmap.width +
                                             pixelX) *
                                            4;
                        bitmap.rgba[offset] = 224;
                        bitmap.rgba[offset + 1] = 240;
                        bitmap.rgba[offset + 2] = 244;
                        bitmap.rgba[offset + 3] = 255;
                    }
                }
            }
        }
        ++column;
    }
    return bitmap;
}

}  // namespace eclipse
