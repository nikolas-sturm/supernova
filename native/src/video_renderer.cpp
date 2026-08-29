#include "video_renderer.h"

#include <stdexcept>

#if defined(_WIN32) && defined(ECLIPSE_HAS_WINDOWS_VIDEO)

#include "input_forwarder.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

namespace eclipse {
namespace {
using Microsoft::WRL::ComPtr;

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}

void requireHresult(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string{message} + " (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)) + ").");
    }
}

}  // namespace

struct VideoRenderer::Impl {
    Impl(StreamSettings streamSettings, StatusListener callback)
        : settings(std::move(streamSettings)), listener(std::move(callback)) {}

    ~Impl() {
        if (codecContext) avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDevice);
        outputView.Reset();
        processor.Reset();
        processorEnumerator.Reset();
        videoContext.Reset();
        videoDevice.Reset();
        if (swapChain && settings.displayMode == DisplayMode::fullscreen) {
            swapChain->SetFullscreenState(FALSE, nullptr);
        }
        swapChain.Reset();
        deviceContext.Reset();
        device.Reset();
        if (window) PostMessageW(window, WM_APP + 1, 0, 0);
        if (windowThread.joinable()) windowThread.join();
    }

    static AVPixelFormat selectPixelFormat(AVCodecContext*, const AVPixelFormat* formats) {
        for (auto format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == AV_PIX_FMT_D3D11) return *format;
        }
        return AV_PIX_FMT_NONE;
    }

    static LRESULT CALLBACK windowProcedure(HWND handle, UINT message, WPARAM word, LPARAM value) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(value);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            LRESULT result = 0;
            if (self->inputForwarder.handleMessage(message, word, value, result)) return result;
        }
        if (message == WM_CLOSE) {
            self->inputForwarder.stop();
            ShowWindow(handle, SW_HIDE);
            if (self && self->listener) {
                self->listener("connected", "Render window hidden. Stop session from Eclipse.");
            }
            return 0;
        }
        if (message == WM_APP + 1) {
            self->inputForwarder.stop();
            DestroyWindow(handle);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(handle, message, word, value);
    }

    void createWindow(int inputWidth, int inputHeight, int frameRate) {
        windowThread = std::thread([this, inputWidth, inputHeight, frameRate] {
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = windowProcedure;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            windowClass.lpszClassName = L"EclipseStreamWindow";
            RegisterClassW(&windowClass);

            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            int positionX = CW_USEDEFAULT;
            int positionY = CW_USEDEFAULT;
            outputWidth = 1280;
            outputHeight = 720;
            if (settings.displayMode == DisplayMode::windowed) {
                RECT workArea{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
                const int maximumWidth = std::max(workArea.right - workArea.left - 80, 640L);
                const int maximumHeight = std::max(workArea.bottom - workArea.top - 120, 360L);
                const double scale = std::min(
                    {1.0, static_cast<double>(maximumWidth) / inputWidth,
                     static_cast<double>(maximumHeight) / inputHeight});
                outputWidth = std::max(static_cast<int>(inputWidth * scale), 640);
                outputHeight = std::max(static_cast<int>(inputHeight * scale), 360);
            } else {
                style = WS_POPUP;
                positionX = 0;
                positionY = 0;
                outputWidth = GetSystemMetrics(SM_CXSCREEN);
                outputHeight = GetSystemMetrics(SM_CYSCREEN);
            }
            RECT bounds{0, 0, outputWidth, outputHeight};
            AdjustWindowRect(&bounds, style, FALSE);
            const auto created = CreateWindowExW(
                0, windowClass.lpszClassName, L"Eclipse Stream", style, positionX,
                positionY, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                nullptr, instance, this);
            {
                std::scoped_lock lock{windowMutex};
                window = created;
                windowReady = true;
            }
            windowCondition.notify_one();
            if (!created) return;
            inputForwarder.start(created, settings.input, inputWidth, inputHeight, frameRate);
            if (settings.keepAwake) {
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            }
            ShowWindow(created, SW_SHOW);
            UpdateWindow(created);
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (settings.keepAwake) SetThreadExecutionState(ES_CONTINUOUS);
            window = nullptr;
        });

        std::unique_lock lock{windowMutex};
        windowCondition.wait(lock, [this] { return windowReady; });
        if (!window) throw std::runtime_error("Cannot create native stream window.");
    }

    void initializeD3d(int inputWidth, int inputHeight, int frameRate) {
        const auto hardwareResult =
            av_hwdevice_ctx_create(&hardwareDevice, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (hardwareResult < 0) {
            throw std::runtime_error("D3D11VA device creation failed: " +
                                     ffmpegError(hardwareResult));
        }
        const auto* hardwareContext = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
        const auto* d3dContext =
            static_cast<AVD3D11VADeviceContext*>(hardwareContext->hwctx);
        device = d3dContext->device;
        deviceContext = d3dContext->device_context;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        requireHresult(device.As(&dxgiDevice), "Cannot query DXGI device");
        requireHresult(dxgiDevice->GetAdapter(&adapter), "Cannot query DXGI adapter");
        requireHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "Cannot query DXGI factory");

        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = static_cast<UINT>(outputWidth);
        swapDescription.Height = static_cast<UINT>(outputHeight);
        swapDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = 2;
        swapDescription.Scaling = DXGI_SCALING_STRETCH;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        requireHresult(factory->CreateSwapChainForHwnd(device.Get(), window, &swapDescription,
                                                       nullptr, nullptr, &swapChain),
                       "Cannot create stream swap chain");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        if (settings.displayMode == DisplayMode::fullscreen) {
            requireHresult(swapChain->SetFullscreenState(TRUE, nullptr),
                           "Cannot enter fullscreen presentation");
        }

        requireHresult(device.As(&videoDevice), "Cannot query D3D11 video device");
        requireHresult(deviceContext.As(&videoContext), "Cannot query D3D11 video context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {static_cast<UINT>(frameRate), 1};
        content.InputWidth = static_cast<UINT>(inputWidth);
        content.InputHeight = static_cast<UINT>(inputHeight);
        content.OutputFrameRate = {static_cast<UINT>(frameRate), 1};
        content.OutputWidth = static_cast<UINT>(outputWidth);
        content.OutputHeight = static_cast<UINT>(outputHeight);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        requireHresult(videoDevice->CreateVideoProcessorEnumerator(&content, &processorEnumerator),
                       "Cannot create D3D11 video processor enumerator");
        requireHresult(videoDevice->CreateVideoProcessor(processorEnumerator.Get(), 0, &processor),
                       "Cannot create D3D11 video processor");

        ComPtr<ID3D11Texture2D> backBuffer;
        requireHresult(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
                       "Cannot get stream back buffer");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDescription{};
        outputDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        requireHresult(videoDevice->CreateVideoProcessorOutputView(
                           backBuffer.Get(), processorEnumerator.Get(), &outputDescription, &outputView),
                       "Cannot create video output view");

        sourceRect = {0, 0, inputWidth, inputHeight};
        const double inputAspect = static_cast<double>(inputWidth) / inputHeight;
        const double outputAspect = static_cast<double>(outputWidth) / outputHeight;
        if (inputAspect > outputAspect) {
            const int height = static_cast<int>(outputWidth / inputAspect);
            const int top = (outputHeight - height) / 2;
            destinationRect = {0, top, outputWidth, top + height};
        } else {
            const int width = static_cast<int>(outputHeight * inputAspect);
            const int left = (outputWidth - width) / 2;
            destinationRect = {left, 0, left + width, outputHeight};
        }
        videoContext->VideoProcessorSetStreamFrameFormat(
            processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destinationRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &destinationRect);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
        inputColorSpace.YCbCr_Matrix = 1;
        inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputColorSpace);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
        outputColorSpace.YCbCr_Matrix = 1;
        outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &outputColorSpace);
    }

    void initializeDecoder(int inputWidth, int inputHeight) {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) throw std::runtime_error("FFmpeg H.264 decoder is unavailable.");
        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) throw std::runtime_error("Cannot allocate FFmpeg decoder context.");
        codecContext->width = inputWidth;
        codecContext->height = inputHeight;
        codecContext->thread_count = 1;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
        codecContext->get_format = selectPixelFormat;
        codecContext->hw_device_ctx = av_buffer_ref(hardwareDevice);
        if (!codecContext->hw_device_ctx) {
            throw std::runtime_error("Cannot attach D3D11VA device to FFmpeg decoder.");
        }
        const auto result = avcodec_open2(codecContext, codec, nullptr);
        if (result < 0) {
            throw std::runtime_error("Cannot open D3D11VA H.264 decoder: " + ffmpegError(result));
        }
    }

    bool render(AVFrame* frame) {
        if (frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) return false;
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
        inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDescription.Texture2D.ArraySlice =
            static_cast<UINT>(reinterpret_cast<std::uintptr_t>(frame->data[1]));
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(
                texture, processorEnumerator.Get(), &inputDescription, &inputView))) {
            return false;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView.Get();
        if (FAILED(videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1,
                                                   &stream))) {
            return false;
        }
        return SUCCEEDED(swapChain->Present(settings.enableVsync ? 1 : 0, 0));
    }

    int submit(PDECODE_UNIT decodeUnit) {
        if (framesDecoded == 0 && decodeUnit->frameType != FRAME_TYPE_IDR) return DR_NEED_IDR;
        AVPacket* packet = av_packet_alloc();
        if (!packet || av_new_packet(packet, decodeUnit->fullLength) < 0) {
            av_packet_free(&packet);
            return DR_NEED_IDR;
        }
        int offset = 0;
        for (auto* entry = decodeUnit->bufferList; entry; entry = entry->next) {
            if (offset + entry->length > packet->size) {
                av_packet_free(&packet);
                return DR_NEED_IDR;
            }
            std::copy_n(reinterpret_cast<const std::uint8_t*>(entry->data), entry->length,
                        packet->data + offset);
            offset += entry->length;
        }
        packet->size = offset;
        if (decodeUnit->frameType == FRAME_TYPE_IDR) packet->flags |= AV_PKT_FLAG_KEY;

        auto result = avcodec_send_packet(codecContext, packet);
        av_packet_free(&packet);
        if (result < 0) {
            if (listener) listener("error", "FFmpeg rejected encoded frame: " + ffmpegError(result));
            return DR_NEED_IDR;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) return DR_NEED_IDR;
        while ((result = avcodec_receive_frame(codecContext, frame)) == 0) {
            if (!render(frame)) {
                av_frame_free(&frame);
                if (listener) listener("error", "D3D11 video presentation failed.");
                return DR_NEED_IDR;
            }
            ++framesDecoded;
            if (framesDecoded == 1 && listener) {
                listener("rendering", "D3D11VA hardware decode and native presentation active.");
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            if (listener) listener("error", "FFmpeg decode failed: " + ffmpegError(result));
            return DR_NEED_IDR;
        }
        return DR_OK;
    }

    StreamSettings settings;
    StatusListener listener;
    std::thread windowThread;
    std::mutex windowMutex;
    std::condition_variable windowCondition;
    HWND window = nullptr;
    bool windowReady = false;
    int outputWidth = 1280;
    int outputHeight = 720;
    InputForwarder inputForwarder;
    AVBufferRef* hardwareDevice = nullptr;
    AVCodecContext* codecContext = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    RECT sourceRect{};
    RECT destinationRect{};
    std::uint64_t framesDecoded = 0;
};

VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener listener)
    : impl_(std::make_unique<Impl>(std::move(settings), std::move(listener))) {}

VideoRenderer::~VideoRenderer() = default;

void VideoRenderer::initialize(int videoFormat, int width, int height, int frameRate) {
    if ((videoFormat & VIDEO_FORMAT_MASK_H264) == 0) {
        throw std::runtime_error("Windows renderer currently supports H.264 streams only.");
    }
    impl_->createWindow(width, height, frameRate);
    impl_->initializeD3d(width, height, frameRate);
    impl_->initializeDecoder(width, height);
}

int VideoRenderer::submit(PDECODE_UNIT decodeUnit) {
    return impl_->submit(decodeUnit);
}

}  // namespace eclipse

#else

namespace eclipse {
struct VideoRenderer::Impl {
    Impl(StreamSettings, StatusListener) {}
};

VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener listener)
    : impl_(std::make_unique<Impl>(std::move(settings), std::move(listener))) {}
VideoRenderer::~VideoRenderer() = default;
void VideoRenderer::initialize(int, int, int, int) {
    throw std::runtime_error("Native video output is unavailable on this platform.");
}
int VideoRenderer::submit(PDECODE_UNIT) {
    return DR_NEED_IDR;
}
}  // namespace eclipse

#endif
