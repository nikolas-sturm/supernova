#include "stream_statistics.h"

#include <chrono>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using namespace std::chrono_literals;
    const eclipse::StreamStatistics::Clock::time_point started{};
    eclipse::StreamStatistics statistics{started};
    statistics.recordVideoUnit(10, 500'000, 20, 1000);
    statistics.recordVideoUnit(12, 500'000, 40, 3000);
    statistics.recordDecoded(2000);
    statistics.recordDecoded(4000);
    statistics.recordPresented(1000, 5000);
    statistics.recordQueueDrop();
    statistics.recordRtt(12, 3);

    expect(!statistics.sampleIfDue(started + 999ms), "Statistics sampled before cadence elapsed.");
    const auto firstSample = statistics.sampleIfDue(started + 1s);
    expect(firstSample.has_value(), "Statistics did not sample after cadence elapsed.");
    const auto& sample = firstSample->statistics;
    expect(sample.totalFps == 3, "Total frame rate is incorrect.");
    expect(sample.receivedFps == 2, "Received frame rate is incorrect.");
    expect(sample.decodedFps == 2, "Decoded frame rate is incorrect.");
    expect(sample.presentedFps == 1, "Presented frame rate is incorrect.");
    expect(sample.bitrateMbps == 8, "Encoded bitrate is incorrect.");
    expect(sample.frameLossPercent > 33 && sample.frameLossPercent < 34,
           "Frame-number gap loss is incorrect.");
    expect(sample.averageHostLatencyMs == 3, "Host latency average is incorrect.");
    expect(sample.minimumHostLatencyMs == 2 && sample.maximumHostLatencyMs == 4,
           "Host latency range is incorrect.");
    expect(sample.averageReassemblyMs == 2, "Reassembly average is incorrect.");
    expect(sample.averageDecodeMs == 3, "Decode average is incorrect.");
    expect(sample.queueDrops == 1, "Queue drop delta is incorrect.");
    expect(sample.jitterLossPercent == 50, "Network jitter loss is incorrect.");
    expect(sample.averageQueueDelayMs == 5, "Frame queue delay is incorrect.");
    expect(sample.rttMs == 12 && sample.rttVarianceMs == 3, "RTT sample is incorrect.");
    const auto cached = statistics.latest();
    expect(cached && cached->sequence == 1 && cached->elapsedMs == 1000,
           "Latest statistics metadata is incorrect.");
    expect(cached->statistics.receivedFps == sample.receivedFps,
           "Reading latest statistics consumed the sample.");
    expect(!statistics.sampleIfDue(started + 1500ms),
           "Statistics sampled twice inside one cadence window.");
    statistics.recordVideoUnit(13, 250'000, 30, 2000);
    const auto secondSample = statistics.sampleIfDue(started + 2s);
    expect(secondSample && secondSample->sequence == 2 && secondSample->statistics.receivedFps == 1,
           "Second statistics window did not contain its delta.");

    const auto text = eclipse::formatStreamStatistics(sample, 1920, 1080, "H.264");
    expect(text.find("Video stream: 1920x1080 3.00 FPS (Codec: H.264)") != std::string::npos,
           "Overlay text omitted stream summary.");
    expect(text.find("Incoming frame rate from network: 2.00 FPS") != std::string::npos,
           "Overlay text omitted incoming frame rate.");
    expect(text.find("Host processing latency min/max/average: 2.0/4.0/3.0 ms") !=
               std::string::npos,
           "Overlay text omitted host latency range.");
    expect(text.find("Frames dropped by your network connection: 33.33%") !=
               std::string::npos,
           "Overlay text omitted network loss.");
    expect(text.find("Frames dropped due to network jitter: 50.00%") != std::string::npos,
           "Overlay text omitted jitter loss.");
    expect(text.find("Average network latency: 12 ms (variance: 3 ms)") != std::string::npos,
           "Overlay text omitted network latency.");
    expect(text.find("Average frame queue delay: 5.00 ms") != std::string::npos,
           "Overlay text omitted frame queue delay.");
    expect(text.find("Average rendering time (including monitor V-sync latency): 1.00 ms") !=
               std::string::npos,
           "Overlay text omitted rendering latency.");
    const auto bitmap = eclipse::rasterizePerformanceOverlay(text);
    expect(bitmap.width > 0 && bitmap.height > 0 &&
               bitmap.rgba.size() == static_cast<std::size_t>(bitmap.width * bitmap.height * 4),
           "Overlay rasterizer produced invalid dimensions.");
}
