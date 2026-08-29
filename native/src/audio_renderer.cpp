#include "audio_renderer.h"

#include <stdexcept>

#if defined(_WIN32) && defined(ECLIPSE_HAS_WINDOWS_VIDEO)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace eclipse {
namespace {
using Microsoft::WRL::ComPtr;

constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr std::size_t kMaximumQueuedSamples = kOutputSampleRate * kOutputChannels / 2;

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}

std::string windowsError(HRESULT error) {
    return "HRESULT " + std::to_string(static_cast<unsigned long>(error));
}

void appendLittleEndian16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendLittleEndian32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}
}  // namespace

struct AudioRenderer::Impl {
    explicit Impl(StatusListener callback) : listener(std::move(callback)) {}

    ~Impl() {
        stopping = true;
        if (audioEvent) SetEvent(audioEvent);
        if (playbackThread.joinable()) playbackThread.join();
        if (audioEvent) CloseHandle(audioEvent);
        swr_free(&resampler);
        if (codecContext) avcodec_free_context(&codecContext);
    }

    void initialize(const OPUS_MULTISTREAM_CONFIGURATION& config) {
        if (config.sampleRate <= 0 || config.channelCount <= 0 || config.channelCount > 8) {
            throw std::runtime_error("Moonlight returned invalid Opus configuration.");
        }
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) throw std::runtime_error("FFmpeg Opus decoder is unavailable.");
        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) throw std::runtime_error("Cannot allocate Opus decoder context.");
        codecContext->sample_rate = config.sampleRate;
        av_channel_layout_default(&codecContext->ch_layout, config.channelCount);

        std::vector<std::uint8_t> opusHead{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
        opusHead.push_back(1);
        opusHead.push_back(static_cast<std::uint8_t>(config.channelCount));
        appendLittleEndian16(opusHead, 0);
        appendLittleEndian32(opusHead, static_cast<std::uint32_t>(config.sampleRate));
        appendLittleEndian16(opusHead, 0);
        const bool standardStereo = config.channelCount <= 2 && config.streams == 1 &&
                                    config.coupledStreams == 1;
        opusHead.push_back(standardStereo ? 0 : 1);
        if (!standardStereo) {
            opusHead.push_back(static_cast<std::uint8_t>(config.streams));
            opusHead.push_back(static_cast<std::uint8_t>(config.coupledStreams));
            opusHead.insert(opusHead.end(), config.mapping,
                            config.mapping + config.channelCount);
        }
        codecContext->extradata_size = static_cast<int>(opusHead.size());
        codecContext->extradata = static_cast<std::uint8_t*>(
            av_mallocz(opusHead.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codecContext->extradata) {
            throw std::runtime_error("Cannot allocate Opus stream configuration.");
        }
        std::copy(opusHead.begin(), opusHead.end(), codecContext->extradata);
        const auto openResult = avcodec_open2(codecContext, codec, nullptr);
        if (openResult < 0) {
            throw std::runtime_error("Cannot open FFmpeg Opus decoder: " +
                                     ffmpegError(openResult));
        }

        playbackThread = std::thread([this] { playbackLoop(); });
        std::unique_lock lock{initializationMutex};
        initializationCondition.wait(lock, [this] { return initializationComplete; });
        if (!initializationError.empty()) throw std::runtime_error(initializationError);
    }

    void finishInitialization(std::string error = {}) {
        {
            std::scoped_lock lock{initializationMutex};
            initializationError = std::move(error);
            initializationComplete = true;
        }
        initializationCondition.notify_one();
    }

    void playbackLoop() {
        const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            finishInitialization("Cannot initialize Windows audio COM: " + windowsError(comResult));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> renderer;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                         &client);

        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = kOutputChannels;
        format.Format.nSamplesPerSec = kOutputSampleRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = kOutputChannels * sizeof(float);
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        format.SubFormat = {WAVE_FORMAT_IEEE_FLOAT,
                            0x0000,
                            0x0010,
                            {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

        if (SUCCEEDED(result)) {
            result = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                1'000'000, 0, &format.Format, nullptr);
        }
        UINT32 bufferFrames = 0;
        if (SUCCEEDED(result)) result = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(result)) {
            audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!audioEvent) result = HRESULT_FROM_WIN32(GetLastError());
        }
        if (SUCCEEDED(result)) result = client->SetEventHandle(audioEvent);
        if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&renderer));
        if (SUCCEEDED(result)) result = client->Start();
        if (FAILED(result)) {
            finishInitialization("Cannot start WASAPI output: " + windowsError(result));
            CoUninitialize();
            return;
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        finishInitialization();

        while (!stopping) {
            WaitForSingleObject(audioEvent, 100);
            if (stopping) break;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= bufferFrames) continue;
            const auto availableFrames = bufferFrames - padding;
            UINT32 queuedFrames = 0;
            {
                std::scoped_lock lock{queueMutex};
                queuedFrames = static_cast<UINT32>(samples.size() / kOutputChannels);
            }
            const auto frames = std::min(availableFrames, queuedFrames);
            if (frames == 0) continue;
            BYTE* destination = nullptr;
            if (FAILED(renderer->GetBuffer(frames, &destination))) continue;
            {
                std::scoped_lock lock{queueMutex};
                auto* output = reinterpret_cast<float*>(destination);
                const auto sampleCount = static_cast<std::size_t>(frames) * kOutputChannels;
                for (std::size_t index = 0; index < sampleCount; ++index) {
                    output[index] = samples.front();
                    samples.pop_front();
                }
            }
            renderer->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        CoUninitialize();
    }

    void queueFrame(AVFrame* frame) {
        if (!resampler) {
            AVChannelLayout stereo;
            av_channel_layout_default(&stereo, kOutputChannels);
            const auto result = swr_alloc_set_opts2(
                &resampler, &stereo, AV_SAMPLE_FMT_FLT, kOutputSampleRate, &frame->ch_layout,
                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&stereo);
            if (result < 0 || swr_init(resampler) < 0) {
                throw std::runtime_error("Cannot initialize FFmpeg audio conversion.");
            }
        }
        const auto capacity = swr_get_out_samples(resampler, frame->nb_samples);
        std::vector<float> converted(static_cast<std::size_t>(capacity) * kOutputChannels);
        auto* output = reinterpret_cast<std::uint8_t*>(converted.data());
        const auto convertedFrames = swr_convert(
            resampler, &output, capacity, const_cast<const std::uint8_t**>(frame->extended_data),
            frame->nb_samples);
        if (convertedFrames < 0) {
            throw std::runtime_error("FFmpeg audio conversion failed: " +
                                     ffmpegError(convertedFrames));
        }
        converted.resize(static_cast<std::size_t>(convertedFrames) * kOutputChannels);
        std::scoped_lock lock{queueMutex};
        const auto overflow = samples.size() + converted.size() > kMaximumQueuedSamples
                                  ? samples.size() + converted.size() - kMaximumQueuedSamples
                                  : 0;
        for (std::size_t index = 0; index < overflow; ++index) samples.pop_front();
        samples.insert(samples.end(), converted.begin(), converted.end());
    }

    void submit(const char* data, int length) {
        if (!data || length <= 0) return;
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, length) < 0) {
            av_packet_free(&packet);
            return;
        }
        std::memcpy(packet->data, data, static_cast<std::size_t>(length));
        auto result = avcodec_send_packet(codecContext, packet);
        av_packet_free(&packet);
        if (result < 0) return;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return;
        while ((result = avcodec_receive_frame(codecContext, frame)) == 0) {
            try {
                queueFrame(frame);
                if (!playbackReported.exchange(true) && listener) {
                    listener("rendering", "D3D11VA video and WASAPI audio playback active.");
                }
            } catch (const std::exception& exception) {
                if (listener) listener("error", exception.what());
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    }

    StatusListener listener;
    AVCodecContext* codecContext = nullptr;
    SwrContext* resampler = nullptr;
    std::thread playbackThread;
    std::atomic_bool stopping{false};
    std::atomic_bool playbackReported{false};
    HANDLE audioEvent = nullptr;
    std::mutex initializationMutex;
    std::condition_variable initializationCondition;
    bool initializationComplete = false;
    std::string initializationError;
    std::mutex queueMutex;
    std::deque<float> samples;
};

AudioRenderer::AudioRenderer(StatusListener listener)
    : impl_(std::make_unique<Impl>(std::move(listener))) {}
AudioRenderer::~AudioRenderer() = default;
void AudioRenderer::initialize(const OPUS_MULTISTREAM_CONFIGURATION& config) {
    impl_->initialize(config);
}
void AudioRenderer::submit(const char* data, int length) {
    impl_->submit(data, length);
}

}  // namespace eclipse

#else

namespace eclipse {
struct AudioRenderer::Impl {
    explicit Impl(StatusListener) {}
};
AudioRenderer::AudioRenderer(StatusListener listener)
    : impl_(std::make_unique<Impl>(std::move(listener))) {}
AudioRenderer::~AudioRenderer() = default;
void AudioRenderer::initialize(const OPUS_MULTISTREAM_CONFIGURATION&) {
    throw std::runtime_error("Native audio output is unavailable on this platform.");
}
void AudioRenderer::submit(const char*, int) {}
}  // namespace eclipse

#endif
