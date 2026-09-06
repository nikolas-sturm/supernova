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
        if (config.sampleRate <= 0 ||
            (config.channelCount != 2 && config.channelCount != 6 && config.channelCount != 8)) {
            throw std::runtime_error("Moonlight returned invalid Opus configuration.");
        }
        outputChannels = config.channelCount;
        outputChannelMask = outputChannels == 8 ? 0x63F : outputChannels == 6 ? 0x3F : 0x3;
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
        format.Format.nChannels = static_cast<WORD>(outputChannels);
        format.Format.nSamplesPerSec = kOutputSampleRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = static_cast<WORD>(outputChannels * sizeof(float));
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = static_cast<DWORD>(outputChannelMask);
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
            const auto waitResult = WaitForSingleObject(audioEvent, 100);
            if (stopping) break;
            if (waitResult == WAIT_FAILED) {
                deviceLost.store(true);
                break;
            }
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                deviceLost.store(true);
                break;
            }
            if (padding >= bufferFrames) continue;
            const auto availableFrames = bufferFrames - padding;
            UINT32 queuedFrames = 0;
            {
                std::scoped_lock lock{queueMutex};
                queuedFrames = static_cast<UINT32>(samples.size() / outputChannels);
            }
            const auto frames = std::min(availableFrames, queuedFrames);
            if (frames == 0) continue;
            BYTE* destination = nullptr;
            if (FAILED(renderer->GetBuffer(frames, &destination))) {
                deviceLost.store(true);
                break;
            }
            {
                std::scoped_lock lock{queueMutex};
                auto* output = reinterpret_cast<float*>(destination);
                const auto sampleCount = static_cast<std::size_t>(frames) * outputChannels;
                for (std::size_t index = 0; index < sampleCount; ++index) {
                    output[index] = samples.front();
                    samples.pop_front();
                }
            }
            if (FAILED(renderer->ReleaseBuffer(frames, 0))) {
                deviceLost.store(true);
                break;
            }
        }
        client->Stop();
        CoUninitialize();
    }

    void queueFrame(AVFrame* frame) {
        if (!resampler) {
            AVChannelLayout stereo;
            av_channel_layout_from_mask(&stereo, outputChannelMask);
            const auto result = swr_alloc_set_opts2(
                &resampler, &stereo, AV_SAMPLE_FMT_FLT, kOutputSampleRate, &frame->ch_layout,
                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&stereo);
            if (result < 0 || swr_init(resampler) < 0) {
                throw std::runtime_error("Cannot initialize FFmpeg audio conversion.");
            }
        }
        const auto capacity = swr_get_out_samples(resampler, frame->nb_samples);
        std::vector<float> converted(static_cast<std::size_t>(capacity) * outputChannels);
        auto* output = reinterpret_cast<std::uint8_t*>(converted.data());
        const auto convertedFrames = swr_convert(
            resampler, &output, capacity, const_cast<const std::uint8_t**>(frame->extended_data),
            frame->nb_samples);
        if (convertedFrames < 0) {
            throw std::runtime_error("FFmpeg audio conversion failed: " +
                                     ffmpegError(convertedFrames));
        }
        converted.resize(static_cast<std::size_t>(convertedFrames) * outputChannels);
        std::scoped_lock lock{queueMutex};
        const auto maximumQueuedSamples =
            static_cast<std::size_t>(kOutputSampleRate * outputChannels / 20);
        if (converted.size() >= maximumQueuedSamples) {
            samples.clear();
            samples.insert(samples.end(), converted.end() - maximumQueuedSamples,
                           converted.end());
            return;
        }
        const auto overflow = samples.size() + converted.size() > maximumQueuedSamples
                                  ? samples.size() + converted.size() - maximumQueuedSamples
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
                if (listener) listener("connected", exception.what());
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
    std::atomic_bool deviceLost{false};
    int outputChannels = 2;
    std::uint64_t outputChannelMask = 0x3;
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
bool AudioRenderer::recoveryRequired() const noexcept { return impl_->deviceLost.load(); }
bool AudioRenderer::supportsOutputChannels(int channels) noexcept {
    if (channels != 2 && channels != 6 && channels != 8) return false;
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(result)) {
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  IID_PPV_ARGS(&client));
    }

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = kOutputSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels == 8 ? 0x63F : channels == 6 ? 0x3F : 0x3;
    format.SubFormat = {WAVE_FORMAT_IEEE_FLOAT,
                        0x0000,
                        0x0010,
                        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    WAVEFORMATEX* closestFormat = nullptr;
    if (SUCCEEDED(result)) {
        result = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &format.Format,
                                           &closestFormat);
    }
    if (closestFormat) CoTaskMemFree(closestFormat);
    client.Reset();
    device.Reset();
    enumerator.Reset();
    if (ownsCom) CoUninitialize();
    return result == S_OK;
}

}  // namespace eclipse

#elif defined(__linux__) && defined(ECLIPSE_HAS_LINUX_AUDIO)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <SDL.h>

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

constexpr int kOutputSampleRate = 48000;

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}

std::string sdlError(const char* message) {
    return std::string{message} + ": " + SDL_GetError();
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
        if (audioDevice) {
            SDL_PauseAudioDevice(audioDevice, 1);
            SDL_CloseAudioDevice(audioDevice);
        }
        swr_free(&resampler);
        if (codecContext) avcodec_free_context(&codecContext);
        if (sdlAudioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    void notify(std::string state, std::string message) noexcept {
        try {
            if (listener) listener(std::move(state), std::move(message));
        } catch (...) {
        }
    }

    static void SDLCALL audioCallback(void* context, Uint8* stream, int length) noexcept {
        SDL_memset(stream, 0, static_cast<std::size_t>(length));
        try {
            static_cast<Impl*>(context)->fillAudio(stream, length);
        } catch (...) {
        }
    }

    void fillAudio(Uint8* stream, int length) {
        const auto requestedSamples = static_cast<std::size_t>(length) / sizeof(float);
        auto* output = reinterpret_cast<float*>(stream);
        std::scoped_lock lock{queueMutex};
        const auto availableSamples = std::min(requestedSamples, samples.size());
        for (std::size_t index = 0; index < availableSamples; ++index) {
            output[index] = samples.front();
            samples.pop_front();
        }
        std::fill(output + availableSamples, output + requestedSamples, 0.0F);
    }

    void initialize(const OPUS_MULTISTREAM_CONFIGURATION& config) {
        if (config.sampleRate <= 0 ||
            (config.channelCount != 2 && config.channelCount != 6 && config.channelCount != 8)) {
            throw std::runtime_error("Moonlight returned invalid Opus configuration.");
        }
        outputChannels = config.channelCount;
        outputChannelMask = outputChannels == 8 ? 0x63F : outputChannels == 6 ? 0x3F : 0x3;
        if (config.samplesPerFrame <= 0 || config.samplesPerFrame > config.sampleRate) {
            throw std::runtime_error("Moonlight returned invalid Opus frame duration.");
        }
        lossOutputFrames = static_cast<int>(
            (static_cast<std::int64_t>(config.samplesPerFrame) * kOutputSampleRate +
             config.sampleRate - 1) /
            config.sampleRate);
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
            opusHead.insert(opusHead.end(), config.mapping, config.mapping + config.channelCount);
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

        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            throw std::runtime_error(sdlError("Cannot initialize SDL audio"));
        }
        sdlAudioInitialized = true;
        SDL_AudioSpec desired{};
        desired.freq = kOutputSampleRate;
        desired.format = AUDIO_F32SYS;
        desired.channels = static_cast<Uint8>(outputChannels);
        desired.samples = 512;
        desired.callback = audioCallback;
        desired.userdata = this;
        SDL_AudioSpec obtained{};
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!audioDevice) throw std::runtime_error(sdlError("Cannot open SDL audio output"));
        if (obtained.freq != kOutputSampleRate || obtained.format != AUDIO_F32SYS ||
            obtained.channels != outputChannels) {
            throw std::runtime_error("SDL audio output did not provide requested channel layout.");
        }
        SDL_PauseAudioDevice(audioDevice, 0);
    }

    void queueFrame(AVFrame* frame) {
        if (!resampler) {
            AVChannelLayout stereo;
            av_channel_layout_from_mask(&stereo, outputChannelMask);
            const auto result = swr_alloc_set_opts2(
                &resampler, &stereo, AV_SAMPLE_FMT_FLT, kOutputSampleRate, &frame->ch_layout,
                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&stereo);
            if (result < 0 || swr_init(resampler) < 0) {
                throw std::runtime_error("Cannot initialize FFmpeg audio conversion.");
            }
        }
        const auto capacity = swr_get_out_samples(resampler, frame->nb_samples);
        std::vector<float> converted(static_cast<std::size_t>(capacity) * outputChannels);
        auto* output = reinterpret_cast<std::uint8_t*>(converted.data());
        const auto convertedFrames = swr_convert(
            resampler, &output, capacity, const_cast<const std::uint8_t**>(frame->extended_data),
            frame->nb_samples);
        if (convertedFrames < 0) {
            throw std::runtime_error("FFmpeg audio conversion failed: " +
                                     ffmpegError(convertedFrames));
        }
        converted.resize(static_cast<std::size_t>(convertedFrames) * outputChannels);
        std::scoped_lock lock{queueMutex};
        const auto maximumQueuedSamples =
            static_cast<std::size_t>(kOutputSampleRate * outputChannels / 20);
        if (converted.size() >= maximumQueuedSamples) {
            samples.clear();
            samples.insert(samples.end(), converted.end() - maximumQueuedSamples,
                           converted.end());
            return;
        }
        const auto overflow = samples.size() + converted.size() > maximumQueuedSamples
                                  ? samples.size() + converted.size() - maximumQueuedSamples
                                  : 0;
        for (std::size_t index = 0; index < overflow; ++index) samples.pop_front();
        samples.insert(samples.end(), converted.begin(), converted.end());
    }

    void queueLossSilence() {
        const auto silenceSamples =
            std::min(static_cast<std::size_t>(lossOutputFrames) * outputChannels,
                     static_cast<std::size_t>(kOutputSampleRate * outputChannels / 20));
        std::scoped_lock lock{queueMutex};
        const auto maximumQueuedSamples =
            static_cast<std::size_t>(kOutputSampleRate * outputChannels / 20);
        const auto overflow = samples.size() + silenceSamples > maximumQueuedSamples
                                  ? samples.size() + silenceSamples - maximumQueuedSamples
                                  : 0;
        for (std::size_t index = 0; index < overflow; ++index) samples.pop_front();
        samples.insert(samples.end(), silenceSamples, 0.0F);
    }

    void concealLoss() noexcept {
        try {
            queueLossSilence();
        } catch (const std::exception& exception) {
            notify("connected", exception.what());
        }
    }

    void submit(const char* data, int length) {
        if (audioDevice && SDL_GetAudioDeviceStatus(audioDevice) == SDL_AUDIO_STOPPED) {
            deviceLost.store(true);
            return;
        }
        if (!data || length <= 0) {
            concealLoss();
            return;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, length) < 0) {
            av_packet_free(&packet);
            concealLoss();
            return;
        }
        std::memcpy(packet->data, data, static_cast<std::size_t>(length));
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            av_packet_free(&packet);
            concealLoss();
            return;
        }
        int result = 0;
        int queuedFrames = 0;
        const auto receiveFrames = [&] {
            while ((result = avcodec_receive_frame(codecContext, frame)) == 0) {
                try {
                    queueFrame(frame);
                    ++queuedFrames;
                    if (!playbackReported.exchange(true)) {
                        notify("rendering", "FFmpeg video and SDL audio playback active.");
                    }
                } catch (const std::exception& exception) {
                    notify("connected", exception.what());
                }
                av_frame_unref(frame);
            }
        };

        result = avcodec_send_packet(codecContext, packet);
        if (result == AVERROR(EAGAIN)) {
            receiveFrames();
            if (result == AVERROR(EAGAIN)) result = avcodec_send_packet(codecContext, packet);
        }
        av_packet_free(&packet);
        if (result < 0) {
            notify("connected", "FFmpeg rejected Opus packet: " + ffmpegError(result));
            av_frame_free(&frame);
            concealLoss();
            return;
        }
        receiveFrames();
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            notify("connected", "FFmpeg Opus decode failed: " + ffmpegError(result));
        }
        if (queuedFrames == 0) concealLoss();
    }

    StatusListener listener;
    AVCodecContext* codecContext = nullptr;
    SwrContext* resampler = nullptr;
    SDL_AudioDeviceID audioDevice = 0;
    int lossOutputFrames = 0;
    bool sdlAudioInitialized = false;
    std::atomic_bool playbackReported{false};
    std::atomic_bool deviceLost{false};
    int outputChannels = 2;
    std::uint64_t outputChannelMask = 0x3;
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
bool AudioRenderer::recoveryRequired() const noexcept { return impl_->deviceLost.load(); }
bool AudioRenderer::supportsOutputChannels(int channels) noexcept {
    if (channels != 2 && channels != 6 && channels != 8) return false;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return false;
    SDL_AudioSpec desired{};
    desired.freq = kOutputSampleRate;
    desired.format = AUDIO_F32SYS;
    desired.channels = static_cast<Uint8>(channels);
    desired.samples = 512;
    SDL_AudioSpec obtained{};
    const auto device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    const bool supported = device != 0 && obtained.freq == desired.freq &&
                           obtained.format == desired.format &&
                           obtained.channels == desired.channels;
    if (device) SDL_CloseAudioDevice(device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return supported;
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
bool AudioRenderer::recoveryRequired() const noexcept { return false; }
bool AudioRenderer::supportsOutputChannels(int channels) noexcept { return channels == 2; }
}  // namespace eclipse

#endif
