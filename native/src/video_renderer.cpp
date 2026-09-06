#include "video_renderer.h"

#include "display_mode.h"
#include "stream_statistics.h"

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace eclipse {
namespace {

bool supportedVideoFormat(int videoFormat) {
    return videoFormat == VIDEO_FORMAT_H264 || videoFormat == VIDEO_FORMAT_H265 ||
           videoFormat == VIDEO_FORMAT_H265_MAIN10 || videoFormat == VIDEO_FORMAT_AV1_MAIN8 ||
           videoFormat == VIDEO_FORMAT_AV1_MAIN10 ||
           videoFormat == VIDEO_FORMAT_H264_HIGH8_444 ||
           videoFormat == VIDEO_FORMAT_H265_REXT8_444 ||
           videoFormat == VIDEO_FORMAT_H265_REXT10_444 ||
           videoFormat == VIDEO_FORMAT_AV1_HIGH8_444 ||
           videoFormat == VIDEO_FORMAT_AV1_HIGH10_444;
}

bool isHevcVideoFormat(int videoFormat) {
    return (videoFormat & VIDEO_FORMAT_MASK_H265) != 0;
}

bool isAv1VideoFormat(int videoFormat) { return (videoFormat & VIDEO_FORMAT_MASK_AV1) != 0; }

bool isHdrVideoFormat(int videoFormat) { return (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0; }

const char* videoCodecName(int videoFormat) {
    const bool yuv444 = (videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    const bool hdr = isHdrVideoFormat(videoFormat);
    if (isAv1VideoFormat(videoFormat)) {
        if (!hdr) return yuv444 ? "AV1 4:4:4" : "AV1";
        if (LiGetCurrentHostDisplayHdrMode()) {
            return yuv444 ? "AV1 10-bit HDR 4:4:4" : "AV1 10-bit HDR";
        }
        return yuv444 ? "AV1 10-bit SDR 4:4:4" : "AV1 10-bit SDR";
    }
    if (isHevcVideoFormat(videoFormat)) {
        if (!hdr) return yuv444 ? "HEVC 4:4:4" : "HEVC";
        if (LiGetCurrentHostDisplayHdrMode()) {
            return yuv444 ? "HEVC 10-bit HDR 4:4:4" : "HEVC 10-bit HDR";
        }
        return yuv444 ? "HEVC 10-bit SDR 4:4:4" : "HEVC 10-bit SDR";
    }
    return yuv444 ? "H.264 4:4:4" : "H.264";
}

}  // namespace
}  // namespace eclipse

#if defined(_WIN32) && defined(ECLIPSE_HAS_WINDOWS_VIDEO)

#include "input_forwarder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
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
constexpr UINT kDestroyWindowMessage = WM_APP + 1;
constexpr UINT kSetInputEnabledMessage = WM_APP + 2;
constexpr UINT kSetGamepadRumbleMessage = WM_APP + 3;
constexpr UINT kSetGamepadMotionMessage = WM_APP + 4;
constexpr UINT kSetGamepadLedMessage = WM_APP + 5;
constexpr UINT kResumeOverlayMessage = WM_APP + 6;

struct DisplayMonitor {
    HMONITOR handle = nullptr;
    MONITORINFOEXW info{};
};

BOOL CALLBACK collectDisplayMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& displays = *reinterpret_cast<std::vector<DisplayMonitor>*>(context);
    DisplayMonitor display;
    display.handle = monitor;
    display.info.cbSize = sizeof(display.info);
    if (GetMonitorInfoW(monitor, &display.info)) displays.push_back(display);
    return TRUE;
}

std::vector<DisplayMonitor> displayMonitors() {
    std::vector<DisplayMonitor> displays;
    EnumDisplayMonitors(nullptr, nullptr, collectDisplayMonitor,
                        reinterpret_cast<LPARAM>(&displays));
    std::stable_sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
        const bool leftPrimary = (left.info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        const bool rightPrimary = (right.info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (leftPrimary != rightPrimary) return leftPrimary;
        return std::wcscmp(left.info.szDevice, right.info.szDevice) < 0;
    });
    return displays;
}

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

int selectVideoFormat(VideoCodec preference, int serverCodecModeSupport, bool enableHdr,
                      bool enableYuv444) {
    if (enableYuv444) {
        throw std::runtime_error("YUV 4:4:4 presentation is unavailable with the Windows renderer.");
    }
    if (preference == VideoCodec::h264 && enableHdr) {
        throw std::runtime_error("HDR requires HEVC or AV1.");
    }

    const auto serverSupports = [serverCodecModeSupport](int format) {
        switch (format) {
            case VIDEO_FORMAT_H264: return (serverCodecModeSupport & SCM_H264) != 0;
            case VIDEO_FORMAT_H265: return (serverCodecModeSupport & SCM_HEVC) != 0;
            case VIDEO_FORMAT_H265_MAIN10:
                return (serverCodecModeSupport & SCM_HEVC_MAIN10) != 0;
            case VIDEO_FORMAT_AV1_MAIN8:
                return (serverCodecModeSupport & SCM_AV1_MAIN8) != 0;
            case VIDEO_FORMAT_AV1_MAIN10:
                return (serverCodecModeSupport & SCM_AV1_MAIN10) != 0;
            default: return false;
        }
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VideoDevice> videoDevice;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr))) {
        static_cast<void>(device.As(&videoDevice));
    }
    const auto clientSupports = [&videoDevice](int format) {
        if (!videoDevice) return false;
        const GUID* profile = nullptr;
        DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
        AVCodecID codecId = AV_CODEC_ID_NONE;
        switch (format) {
            case VIDEO_FORMAT_H264:
                profile = &D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
                textureFormat = DXGI_FORMAT_NV12;
                codecId = AV_CODEC_ID_H264;
                break;
            case VIDEO_FORMAT_H265:
                profile = &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
                textureFormat = DXGI_FORMAT_NV12;
                codecId = AV_CODEC_ID_HEVC;
                break;
            case VIDEO_FORMAT_H265_MAIN10:
                profile = &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10;
                textureFormat = DXGI_FORMAT_P010;
                codecId = AV_CODEC_ID_HEVC;
                break;
            case VIDEO_FORMAT_AV1_MAIN8:
                profile = &D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
                textureFormat = DXGI_FORMAT_NV12;
                codecId = AV_CODEC_ID_AV1;
                break;
            case VIDEO_FORMAT_AV1_MAIN10:
                profile = &D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0;
                textureFormat = DXGI_FORMAT_P010;
                codecId = AV_CODEC_ID_AV1;
                break;
            default: return false;
        }
        BOOL supported = FALSE;
        return avcodec_find_decoder(codecId) &&
               SUCCEEDED(videoDevice->CheckVideoDecoderFormat(profile, textureFormat, &supported)) &&
               supported;
    };

    const auto requireFormat = [&](int format) {
        if (!serverSupports(format)) {
            throw std::runtime_error(std::string{"Host does not support "} +
                                     videoCodecName(format) + " streaming.");
        }
        if (!clientSupports(format)) {
            throw std::runtime_error(std::string{"Client GPU does not support D3D11VA "} +
                                     videoCodecName(format) + " decoding.");
        }
        return format;
    };

    if (preference == VideoCodec::h264) return requireFormat(VIDEO_FORMAT_H264);

    if (preference == VideoCodec::hevc) {
        return requireFormat(enableHdr ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265);
    }
    if (preference == VideoCodec::av1) {
        return requireFormat(enableHdr ? VIDEO_FORMAT_AV1_MAIN10 : VIDEO_FORMAT_AV1_MAIN8);
    }

    const int hevcFormat = enableHdr ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265;
    if (serverSupports(hevcFormat) && clientSupports(hevcFormat)) return hevcFormat;
    const int av1Format = enableHdr ? VIDEO_FORMAT_AV1_MAIN10 : VIDEO_FORMAT_AV1_MAIN8;
    if (serverSupports(av1Format) && clientSupports(av1Format)) return av1Format;
    if (enableHdr) {
        throw std::runtime_error("No mutually supported HDR10 video codec is available.");
    }
    return requireFormat(VIDEO_FORMAT_H264);
}

struct VideoRenderer::Impl {
    Impl(StreamSettings streamSettings, StatusListener callback, CloseListener closeCallback,
          std::shared_ptr<StreamStatistics> streamStatistics, OverlayListener overlayCallback)
        : settings(std::move(streamSettings)),
          listener(std::move(callback)),
          closeListener(std::move(closeCallback)),
          statistics(std::move(streamStatistics)),
          overlayListener(std::move(overlayCallback)) {
        hdrModeRequested.store(settings.enableHdr);
        overlayVisible.store(settings.showPerformanceStats);
    }

    ~Impl() {
        if (codecContext) avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDevice);
        outputView.Reset();
        processor.Reset();
        processorEnumerator.Reset();
        videoContext.Reset();
        videoDevice.Reset();
        if (swapChain && exclusiveFullscreen) {
            if (FAILED(swapChain->SetFullscreenState(FALSE, nullptr))) {
                Sleep(50);
                static_cast<void>(swapChain->SetFullscreenState(FALSE, nullptr));
            }
        }
        hdrSwapChain.Reset();
        swapChain.Reset();
        deviceContext.Reset();
        device.Reset();
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (handle) PostMessageW(handle, kDestroyWindowMessage, 0, 0);
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
            if (message == kSetInputEnabledMessage) {
                self->inputForwarder.setEnabled(word != 0);
                return 0;
            }
            if (message == kSetGamepadRumbleMessage) {
                const auto motors = static_cast<std::uint32_t>(value);
                self->inputForwarder.setGamepadRumble(
                    static_cast<std::uint16_t>(word),
                    static_cast<std::uint16_t>(motors >> 16),
                    static_cast<std::uint16_t>(motors & 0xffff));
                return 0;
            }
            if (message == kSetGamepadMotionMessage) {
                const auto state = static_cast<std::uint32_t>(value);
                self->inputForwarder.setGamepadMotionEventState(
                    static_cast<std::uint16_t>(word), static_cast<std::uint8_t>(state >> 16U),
                    static_cast<std::uint16_t>(state));
                return 0;
            }
            if (message == kSetGamepadLedMessage) {
                const auto color = static_cast<std::uint32_t>(value);
                self->inputForwarder.setGamepadLed(
                    static_cast<std::uint16_t>(word), static_cast<std::uint8_t>(color >> 16U),
                    static_cast<std::uint8_t>(color >> 8U), static_cast<std::uint8_t>(color));
                return 0;
            }
            if (message == kResumeOverlayMessage) {
                ShowWindow(handle, SW_RESTORE);
                SetForegroundWindow(handle);
                SetFocus(handle);
                self->inputForwarder.resumeAfterOverlay();
                return 0;
            }
            LRESULT result = 0;
            if (self->inputForwarder.handleMessage(message, word, value, result)) return result;
            if (message == WM_EXITSIZEMOVE) {
                self->requestDisplayRecovery(handle, false);
            } else if (message == WM_DISPLAYCHANGE) {
                self->requestDisplayRecovery(handle, true);
            }
        }
        if (message == WM_CLOSE) {
            self->inputForwarder.stop();
            ShowWindow(handle, SW_HIDE);
            if (self && !self->closeRequested.exchange(true) && self->closeListener) {
                self->closeListener();
            }
            return 0;
        }
        if (message == kDestroyWindowMessage) {
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

    void requestDisplayRecovery(HWND handle, bool topologyChanged) {
        if (!presentationInitialized.load()) return;
        const auto monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
        if (!topologyChanged && monitor == selectedDisplayHandle) return;
        const auto displays = displayMonitors();
        const auto display = std::find_if(displays.begin(), displays.end(), [&](const auto& value) {
            return value.handle == monitor;
        });
        recoveryDisplayIndex.store(display == displays.end()
                                       ? 0
                                       : static_cast<int>(std::distance(displays.begin(), display)));
        recoveryRequested.store(true);
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

            auto displays = displayMonitors();
            if (displays.empty()) {
                DisplayMonitor display;
                display.handle = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
                display.info.cbSize = sizeof(display.info);
                GetMonitorInfoW(display.handle, &display.info);
                displays.push_back(display);
            }
            const auto displayIndex = settings.displayIndex >= 0 &&
                                              settings.displayIndex < static_cast<int>(displays.size())
                                          ? settings.displayIndex
                                          : 0;
            const auto& targetDisplay = displays[displayIndex];
            selectedDisplayHandle = targetDisplay.handle;
            if (displayIndex != settings.displayIndex && listener) {
                listener("connected", "Selected display is unavailable; using primary display.");
            }

            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            int positionX = targetDisplay.info.rcWork.left;
            int positionY = targetDisplay.info.rcWork.top;
            outputWidth = 1280;
            outputHeight = 720;
            if (settings.displayMode == DisplayMode::windowed) {
                const auto& workArea = targetDisplay.info.rcWork;
                const int maximumWidth = std::max(workArea.right - workArea.left - 80, 640L);
                const int maximumHeight = std::max(workArea.bottom - workArea.top - 120, 360L);
                const double scale = std::min(
                    {1.0, static_cast<double>(maximumWidth) / inputWidth,
                     static_cast<double>(maximumHeight) / inputHeight});
                outputWidth = std::max(static_cast<int>(inputWidth * scale), 640);
                outputHeight = std::max(static_cast<int>(inputHeight * scale), 360);
            } else {
                style = WS_POPUP;
                positionX = targetDisplay.info.rcMonitor.left;
                positionY = targetDisplay.info.rcMonitor.top;
                outputWidth = targetDisplay.info.rcMonitor.right - targetDisplay.info.rcMonitor.left;
                outputHeight = targetDisplay.info.rcMonitor.bottom - targetDisplay.info.rcMonitor.top;
            }
            RECT bounds{0, 0, outputWidth, outputHeight};
            AdjustWindowRect(&bounds, style, FALSE);
            if (settings.displayMode == DisplayMode::windowed) {
                const auto& workArea = targetDisplay.info.rcWork;
                positionX = workArea.left +
                            ((workArea.right - workArea.left) - (bounds.right - bounds.left)) / 2;
                positionY = workArea.top +
                            ((workArea.bottom - workArea.top) - (bounds.bottom - bounds.top)) / 2;
            }
            const auto created = CreateWindowExW(
                0, windowClass.lpszClassName, L"Eclipse Stream", style, positionX,
                positionY, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                nullptr, instance, this);
            {
                std::scoped_lock lock{windowMutex};
                window = created;
            }
            if (!created) {
                {
                    std::scoped_lock lock{windowMutex};
                    windowReady = true;
                }
                windowCondition.notify_one();
                return;
            }
            try {
                inputForwarder.start(created, settings.input, inputWidth, inputHeight, frameRate,
                                     [this] {
                                         overlayVisible.store(!overlayVisible.load());
                                     },
                                     [this] { return toggleFullscreen(); },
                                     [this] { requestOverlay(); });
            } catch (const std::exception& exception) {
                DestroyWindow(created);
                {
                    std::scoped_lock lock{windowMutex};
                    window = nullptr;
                    windowError = exception.what();
                    windowReady = true;
                }
                windowCondition.notify_one();
                return;
            }
            if (settings.keepAwake) {
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            }
            ShowWindow(created, SW_SHOW);
            UpdateWindow(created);
            DEVMODEW displayMode{};
            displayMode.dmSize = sizeof(displayMode);
            const bool hasRefreshRate = EnumDisplaySettingsW(targetDisplay.info.szDevice,
                                                               ENUM_CURRENT_SETTINGS, &displayMode) &&
                                          displayMode.dmDisplayFrequency > 1;
            displayRefreshRate =
                hasRefreshRate ? static_cast<int>(displayMode.dmDisplayFrequency) : 0;
            presentWithVsync = settings.enableVsync &&
                                (!hasRefreshRate ||
                                 frameRate <= static_cast<int>(displayMode.dmDisplayFrequency) + 1);
            {
                std::scoped_lock lock{windowMutex};
                windowReady = true;
            }
            windowCondition.notify_one();
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (settings.keepAwake) SetThreadExecutionState(ES_CONTINUOUS);
            {
                std::scoped_lock lock{windowMutex};
                window = nullptr;
            }
        });

        std::unique_lock lock{windowMutex};
        windowCondition.wait(lock, [this] { return windowReady; });
        if (!window) {
            throw std::runtime_error(windowError.empty() ? "Cannot create native stream window."
                                                         : windowError);
        }
    }

    bool applyHdrMode(bool enabled) {
        if (!videoContext1 || !hdrSwapChain) return !enabled;
        const auto inputColorSpace = enabled ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
                                             : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        const auto outputColorSpace = enabled ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        UINT support = 0;
        if (FAILED(hdrSwapChain->CheckColorSpaceSupport(outputColorSpace, &support)) ||
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
            return false;
        }
        videoContext1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, inputColorSpace);
        videoContext1->VideoProcessorSetOutputColorSpace1(processor.Get(), outputColorSpace);
        if (FAILED(hdrSwapChain->SetColorSpace1(outputColorSpace))) return false;
        appliedHdrMode = enabled;
        hdrModeInitialized = true;
        return true;
    }

    bool toggleFullscreen() {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (!handle) return settings.input.fullscreen;
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE));
        const bool fullscreen = (style & WS_CAPTION) == 0;
        if (fullscreen) {
            if (exclusiveFullscreen && swapChain) {
                static_cast<void>(swapChain->SetFullscreenState(FALSE, nullptr));
                exclusiveFullscreen = false;
            }
            const DWORD restoredStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            SetWindowLongPtrW(handle, GWL_STYLE, restoredStyle);
            RECT bounds = windowedBounds;
            if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
                MONITORINFO monitor{.cbSize = sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
                const int width = std::min(1280L, monitor.rcWork.right - monitor.rcWork.left);
                const int height = std::min(720L, monitor.rcWork.bottom - monitor.rcWork.top);
                bounds = {
                    monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                    monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
                    0,
                    0,
                };
                bounds.right = bounds.left + width;
                bounds.bottom = bounds.top + height;
            }
            SetWindowPos(handle, HWND_NOTOPMOST, bounds.left, bounds.top,
                         bounds.right - bounds.left, bounds.bottom - bounds.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            settings.input.fullscreen = false;
            return false;
        }

        GetWindowRect(handle, &windowedBounds);
        MONITORINFO monitor{.cbSize = sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(handle, GWL_STYLE, WS_POPUP);
        SetWindowPos(handle, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        settings.input.fullscreen = true;
        return true;
    }

    void requestOverlay() {
        if (!overlayListener || !window) return;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return;
        const auto dpi = GetDpiForWindow(window);
        const double scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
        overlayListener({
            .x = static_cast<int>(std::lround(bounds.left / scale)),
            .y = static_cast<int>(std::lround(bounds.top / scale)),
            .width = std::max(static_cast<int>(std::lround((bounds.right - bounds.left) / scale)),
                              1),
            .height = std::max(static_cast<int>(std::lround((bounds.bottom - bounds.top) / scale)),
                               1),
            .scaleFactor = scale,
            .wayland = false,
            .fullscreen = settings.input.fullscreen,
        });
    }

    void resumeOverlay() {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (handle) PostMessageW(handle, kResumeOverlayMessage, 0, 0);
    }

    void initializeD3d(int videoFormat, int inputWidth, int inputHeight, int frameRate) {
        ComPtr<IDXGIFactory1> discoveryFactory;
        ComPtr<IDXGIAdapter1> selectedAdapter;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&discoveryFactory)))) {
            for (UINT adapterIndex = 0; !selectedAdapter; ++adapterIndex) {
                ComPtr<IDXGIAdapter1> candidate;
                if (discoveryFactory->EnumAdapters1(adapterIndex, &candidate) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                for (UINT outputIndex = 0;; ++outputIndex) {
                    ComPtr<IDXGIOutput> output;
                    if (candidate->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
                    DXGI_OUTPUT_DESC description{};
                    if (output && SUCCEEDED(output->GetDesc(&description)) &&
                        description.Monitor == selectedDisplayHandle) {
                        selectedAdapter = candidate;
                        break;
                    }
                }
            }
        }

        const auto createResult = D3D11CreateDevice(
            selectedAdapter.Get(), selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &deviceContext);
        requireHresult(createResult, "Cannot create D3D11 device for selected display");

        hardwareDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hardwareDevice) throw std::runtime_error("Cannot allocate D3D11VA device context.");
        auto* hardwareContext = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
        auto* d3dContext = static_cast<AVD3D11VADeviceContext*>(hardwareContext->hwctx);
        device.CopyTo(&d3dContext->device);
        deviceContext.CopyTo(&d3dContext->device_context);
        const auto hardwareResult = av_hwdevice_ctx_init(hardwareDevice);
        if (hardwareResult < 0) {
            throw std::runtime_error("D3D11VA device creation failed: " +
                                     ffmpegError(hardwareResult));
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        requireHresult(device.As(&dxgiDevice), "Cannot query DXGI device");
        requireHresult(dxgiDevice->GetAdapter(&adapter), "Cannot query DXGI adapter");
        requireHresult(adapter->GetParent(IID_PPV_ARGS(&factory)), "Cannot query DXGI factory");

        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = static_cast<UINT>(outputWidth);
        swapDescription.Height = static_cast<UINT>(outputHeight);
        swapDescription.Format = isHdrVideoFormat(videoFormat) ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                               : DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = 2;
        swapDescription.Scaling = DXGI_SCALING_STRETCH;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDescription.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        ComPtr<IDXGISwapChain1> createdSwapChain;
        requireHresult(factory->CreateSwapChainForHwnd(device.Get(), window, &swapDescription,
                                                       nullptr, nullptr, &createdSwapChain),
                        "Cannot create stream swap chain");
        swapChain = createdSwapChain;
        if (isHdrVideoFormat(videoFormat)) {
            requireHresult(swapChain.As(&hdrSwapChain), "Cannot query HDR-capable swap chain");
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        if (settings.displayMode == DisplayMode::fullscreen) {
            ComPtr<IDXGIOutput> targetOutput;
            for (UINT outputIndex = 0;; ++outputIndex) {
                ComPtr<IDXGIOutput> output;
                const auto enumerationResult = adapter->EnumOutputs(outputIndex, &output);
                if (FAILED(enumerationResult)) break;
                DXGI_OUTPUT_DESC description{};
                if (output && SUCCEEDED(output->GetDesc(&description)) &&
                    description.Monitor == selectedDisplayHandle) {
                    targetOutput = std::move(output);
                    break;
                }
            }
            if (targetOutput && SUCCEEDED(swapChain->SetFullscreenState(TRUE, targetOutput.Get()))) {
                exclusiveFullscreen = true;
                int selectedWidth = outputWidth;
                int selectedHeight = outputHeight;
                UINT modeCount = 0;
                if (SUCCEEDED(targetOutput->GetDisplayModeList(swapDescription.Format, 0,
                                                               &modeCount, nullptr)) &&
                    modeCount > 0) {
                    std::vector<DXGI_MODE_DESC> modes(modeCount);
                    if (SUCCEEDED(targetOutput->GetDisplayModeList(swapDescription.Format, 0,
                                                                   &modeCount, modes.data()))) {
                        std::vector<DisplayModeSpec> modeSpecs;
                        modeSpecs.reserve(modeCount);
                        for (UINT index = 0; index < modeCount; ++index) {
                            const auto denominator = modes[index].RefreshRate.Denominator;
                            const auto refreshRate = denominator == 0
                                                         ? 0
                                                         : static_cast<int>(
                                                               (modes[index].RefreshRate.Numerator +
                                                                denominator / 2) /
                                                               denominator);
                            modeSpecs.push_back({static_cast<int>(modes[index].Width),
                                                 static_cast<int>(modes[index].Height), refreshRate});
                        }
                        const auto selected = selectOptimalDisplayMode(
                            modeSpecs, {outputWidth, outputHeight, displayRefreshRate}, inputWidth,
                            inputHeight, frameRate);
                        UINT selectedIndex = modeCount;
                        double closestRefresh = std::numeric_limits<double>::max();
                        for (UINT index = 0; index < modeCount; ++index) {
                            if (modeSpecs[index] != selected ||
                                modes[index].RefreshRate.Denominator == 0) {
                                continue;
                            }
                            const auto refresh =
                                static_cast<double>(modes[index].RefreshRate.Numerator) /
                                modes[index].RefreshRate.Denominator;
                            const auto difference = std::abs(refresh - selected.refreshRate);
                            if (difference < closestRefresh) {
                                selectedIndex = index;
                                closestRefresh = difference;
                            }
                        }
                        if (selectedIndex < modeCount &&
                            SUCCEEDED(swapChain->ResizeTarget(&modes[selectedIndex]))) {
                            displayRefreshRate = selected.refreshRate;
                            selectedWidth = selected.width;
                            selectedHeight = selected.height;
                        }
                    }
                }
                if (SUCCEEDED(swapChain->ResizeBuffers(
                        0, static_cast<UINT>(selectedWidth), static_cast<UINT>(selectedHeight),
                        DXGI_FORMAT_UNKNOWN, swapDescription.Flags))) {
                    outputWidth = selectedWidth;
                    outputHeight = selectedHeight;
                } else {
                    requireHresult(swapChain->SetFullscreenState(FALSE, nullptr),
                                   "Cannot leave failed fullscreen transition");
                    exclusiveFullscreen = false;
                    requireHresult(swapChain->ResizeBuffers(
                                       0, static_cast<UINT>(outputWidth),
                                       static_cast<UINT>(outputHeight), DXGI_FORMAT_UNKNOWN,
                                       swapDescription.Flags),
                                   "Cannot restore windowed swap-chain buffers");
                    if (listener) {
                        listener("connected",
                                 "Exclusive fullscreen buffer transition failed; using borderless fullscreen.");
                    }
                }
            } else if (listener) {
                listener("connected",
                         "Exclusive fullscreen unavailable on selected display; using borderless fullscreen.");
            }
        }
        presentWithVsync = settings.enableVsync &&
                           (displayRefreshRate <= 1 || frameRate <= displayRefreshRate + 1);
        if (settings.enableVsync && !presentWithVsync && listener) {
            listener("connected", "V-Sync bypassed because stream FPS exceeds display refresh rate.");
        }

        requireHresult(device.As(&videoDevice), "Cannot query D3D11 video device");
        requireHresult(deviceContext.As(&videoContext), "Cannot query D3D11 video context");
        if (isHdrVideoFormat(videoFormat)) {
            requireHresult(videoContext.As(&videoContext1),
                           "Cannot query HDR-capable D3D11 video context");
        }
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
        if (statistics) {
            try {
                initializePerformanceOverlay(backBuffer.Get());
            } catch (const std::exception& exception) {
                overlayDisabled = true;
                if (listener) {
                    listener("connected", std::string{"Performance overlay unavailable: "} +
                                              exception.what());
                }
            }
        }

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

        if (isHdrVideoFormat(videoFormat) && !applyHdrMode(settings.enableHdr)) {
            throw std::runtime_error(
                "HDR10 output is unavailable. Enable HDR in Windows display settings.");
        }
        presentationInitialized.store(true);
    }

    void initializePerformanceOverlay(ID3D11Texture2D* backBuffer) {
        static constexpr char vertexSource[] = R"(
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    Output output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
})";
        static constexpr char pixelSource[] = R"(
Texture2D overlay : register(t0);
SamplerState overlaySampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return overlay.Sample(overlaySampler, uv);
})";
        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> pixelBytecode;
        ComPtr<ID3DBlob> errors;
        requireHresult(D3DCompile(vertexSource, sizeof(vertexSource), nullptr, nullptr, nullptr,
                                  "main", "vs_4_0", 0, 0, &vertexBytecode, &errors),
                         "Cannot compile performance overlay vertex shader");
        errors.Reset();
        requireHresult(D3DCompile(pixelSource, sizeof(pixelSource), nullptr, nullptr, nullptr,
                                  "main", "ps_4_0", 0, 0, &pixelBytecode, &errors),
                         "Cannot compile performance overlay pixel shader");
        requireHresult(device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                                  vertexBytecode->GetBufferSize(), nullptr,
                                                  &overlayVertexShader),
                         "Cannot create performance overlay vertex shader");
        requireHresult(device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                                 pixelBytecode->GetBufferSize(), nullptr,
                                                 &overlayPixelShader),
                         "Cannot create performance overlay pixel shader");
        requireHresult(device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget),
                         "Cannot create performance overlay render target");

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        requireHresult(device->CreateSamplerState(&samplerDescription, &overlaySampler),
                         "Cannot create performance overlay sampler");

        D3D11_BLEND_DESC blendDescription{};
        blendDescription.RenderTarget[0].BlendEnable = TRUE;
        blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        requireHresult(device->CreateBlendState(&blendDescription, &overlayBlend),
                         "Cannot create performance overlay blend state");
        nextOverlayUpdate = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }

    bool updatePerformanceOverlay() {
        const auto now = std::chrono::steady_clock::now();
        if (now < nextOverlayUpdate) return true;
        const auto sample = statistics->latest();
        if (!sample) return true;
        const auto text = formatStreamStatistics(sample->statistics, sourceRect.right,
                                                   sourceRect.bottom,
                                                   videoCodecName(activeVideoFormat));
        const auto bitmap = rasterizePerformanceOverlay(text);
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = static_cast<UINT>(bitmap.width);
        textureDescription.Height = static_cast<UINT>(bitmap.height);
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA textureData{};
        textureData.pSysMem = bitmap.rgba.data();
        textureData.SysMemPitch = static_cast<UINT>(bitmap.width * 4);
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&textureDescription, &textureData, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &overlayView))) {
            return false;
        }
        overlayWidth = bitmap.width;
        overlayHeight = bitmap.height;
        nextOverlayUpdate = now + std::chrono::seconds{1};
        return true;
    }

    bool drawPerformanceOverlay() {
        if (!statistics || !overlayVisible.load() || overlayDisabled) return true;
        if (!updatePerformanceOverlay()) {
            overlayDisabled = true;
            if (listener) listener("rendering", "Performance overlay was disabled after a D3D11 error.");
            return true;
        }
        if (!overlayView) return true;
        D3D11_VIEWPORT viewport{10.0F, 10.0F, static_cast<float>(overlayWidth),
                               static_cast<float>(overlayHeight), 0.0F, 1.0F};
        ID3D11RenderTargetView* target = renderTarget.Get();
        ID3D11ShaderResourceView* view = overlayView.Get();
        ID3D11SamplerState* sampler = overlaySampler.Get();
        const float blendFactor[4]{};
        deviceContext->OMSetRenderTargets(1, &target, nullptr);
        deviceContext->OMSetBlendState(overlayBlend.Get(), blendFactor, 0xFFFFFFFF);
        deviceContext->RSSetViewports(1, &viewport);
        deviceContext->IASetInputLayout(nullptr);
        deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deviceContext->VSSetShader(overlayVertexShader.Get(), nullptr, 0);
        deviceContext->PSSetShader(overlayPixelShader.Get(), nullptr, 0);
        deviceContext->PSSetShaderResources(0, 1, &view);
        deviceContext->PSSetSamplers(0, 1, &sampler);
        deviceContext->Draw(3, 0);
        view = nullptr;
        deviceContext->PSSetShaderResources(0, 1, &view);
        return true;
    }

    void setInputEnabled(bool enabled) {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (handle) SendMessageW(handle, kSetInputEnabledMessage, enabled ? 1 : 0, 0);
    }

    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                          std::uint16_t highFrequency) {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        const auto motors = (static_cast<std::uint32_t>(lowFrequency) << 16) | highFrequency;
        if (handle) {
            SendMessageW(handle, kSetGamepadRumbleMessage, controllerNumber,
                         static_cast<LPARAM>(motors));
        }
    }

    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz) {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (handle) {
            SendMessageW(handle, kSetGamepadMotionMessage, controllerNumber,
                         static_cast<LPARAM>((static_cast<std::uint32_t>(motionType) << 16U) |
                                            reportRateHz));
        }
    }

    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue) {
        HWND handle = nullptr;
        {
            std::scoped_lock lock{windowMutex};
            handle = window;
        }
        if (handle) {
            SendMessageW(handle, kSetGamepadLedMessage, controllerNumber,
                         static_cast<LPARAM>((static_cast<std::uint32_t>(red) << 16U) |
                                            (static_cast<std::uint32_t>(green) << 8U) | blue));
        }
    }

    void initializeDecoder(int videoFormat, int inputWidth, int inputHeight) {
        const AVCodecID codecId = isAv1VideoFormat(videoFormat)   ? AV_CODEC_ID_AV1
                                  : isHevcVideoFormat(videoFormat) ? AV_CODEC_ID_HEVC
                                                                   : AV_CODEC_ID_H264;
        const auto* codec = avcodec_find_decoder(codecId);
        if (!codec) {
            throw std::runtime_error(std::string{"FFmpeg "} + videoCodecName(videoFormat) +
                                     " decoder is unavailable.");
        }
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
            throw std::runtime_error(std::string{"Cannot open D3D11VA "} +
                                     videoCodecName(videoFormat) + " decoder: " +
                                     ffmpegError(result));
        }
        activeVideoFormat = videoFormat;
    }

    bool render(AVFrame* frame) {
        const auto renderStarted = std::chrono::steady_clock::now();
        if (frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) return false;
        if (isHdrVideoFormat(activeVideoFormat)) {
            const bool requestedHdrMode = hdrModeRequested.load();
            if ((!hdrModeInitialized || requestedHdrMode != appliedHdrMode) &&
                !applyHdrMode(requestedHdrMode)) {
                return false;
            }
        }
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
        if (!drawPerformanceOverlay() || FAILED(swapChain->Present(presentWithVsync ? 1 : 0, 0))) {
            return false;
        }
        if (statistics) {
            statistics->recordPresented(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - renderStarted)
                    .count()));
        }
        return true;
    }

    void setHdrMode(bool enabled) {
        if (isHdrVideoFormat(activeVideoFormat)) hdrModeRequested.store(enabled);
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

        auto decodeStarted = std::chrono::steady_clock::now();
        auto result = avcodec_send_packet(codecContext, packet);
        av_packet_free(&packet);
        if (result < 0) {
            recoveryRequested.store(true);
            if (listener) listener("connected", "FFmpeg rejected encoded frame: " + ffmpegError(result));
            return DR_NEED_IDR;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) return DR_NEED_IDR;
        while (true) {
            result = avcodec_receive_frame(codecContext, frame);
            if (result != 0) break;
            if (statistics) {
                statistics->recordDecoded(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - decodeStarted)
                        .count()));
            }
            if (!render(frame)) {
                recoveryRequested.store(true);
                av_frame_free(&frame);
                if (listener) listener("connected", "D3D11 video presentation failed.");
                return DR_NEED_IDR;
            }
            ++framesDecoded;
            if (framesDecoded == 1 && listener) {
                listener("rendering", std::string{"D3D11VA "} +
                                          videoCodecName(activeVideoFormat) +
                                          " hardware decode and native presentation active.");
            }
            av_frame_unref(frame);
            decodeStarted = std::chrono::steady_clock::now();
        }
        av_frame_free(&frame);
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            recoveryRequested.store(true);
            if (listener) listener("connected", "FFmpeg decode failed: " + ffmpegError(result));
            return DR_NEED_IDR;
        }
        return DR_OK;
    }

    StreamSettings settings;
    StatusListener listener;
    CloseListener closeListener;
    std::shared_ptr<StreamStatistics> statistics;
    OverlayListener overlayListener;
    std::thread windowThread;
    std::mutex windowMutex;
    std::condition_variable windowCondition;
    HWND window = nullptr;
    bool windowReady = false;
    std::string windowError;
    int outputWidth = 1280;
    int outputHeight = 720;
    InputForwarder inputForwarder;
    AVBufferRef* hardwareDevice = nullptr;
    AVCodecContext* codecContext = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<IDXGISwapChain3> hdrSwapChain;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoContext1> videoContext1;
    ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11VertexShader> overlayVertexShader;
    ComPtr<ID3D11PixelShader> overlayPixelShader;
    ComPtr<ID3D11SamplerState> overlaySampler;
    ComPtr<ID3D11BlendState> overlayBlend;
    ComPtr<ID3D11ShaderResourceView> overlayView;
    RECT sourceRect{};
    RECT destinationRect{};
    int activeVideoFormat = VIDEO_FORMAT_H264;
    std::atomic_bool hdrModeRequested{false};
    bool hdrModeInitialized = false;
    bool appliedHdrMode = false;
    bool presentWithVsync = false;
    HMONITOR selectedDisplayHandle = nullptr;
    int displayRefreshRate = 0;
    bool exclusiveFullscreen = false;
    std::atomic_bool closeRequested{false};
    std::atomic_bool recoveryRequested{false};
    std::atomic_bool presentationInitialized{false};
    std::atomic_int recoveryDisplayIndex{-1};
    std::uint64_t framesDecoded = 0;
    int overlayWidth = 0;
    int overlayHeight = 0;
    std::chrono::steady_clock::time_point nextOverlayUpdate{};
    bool overlayDisabled = false;
    std::atomic_bool overlayVisible{false};
    RECT windowedBounds{};
};

VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener listener,
                               CloseListener closeListener,
                               std::shared_ptr<StreamStatistics> statistics,
                               OverlayListener overlayListener)
    : impl_(std::make_unique<Impl>(std::move(settings), std::move(listener),
                                     std::move(closeListener), std::move(statistics),
                                     std::move(overlayListener))) {}

VideoRenderer::~VideoRenderer() = default;

void VideoRenderer::initialize(int videoFormat, int width, int height, int frameRate) {
    if (!supportedVideoFormat(videoFormat)) {
        throw std::runtime_error("Windows renderer received an unsupported video format.");
    }
    impl_->activeVideoFormat = videoFormat;
    impl_->hdrModeRequested.store(impl_->settings.enableHdr);
    impl_->createWindow(width, height, frameRate);
    impl_->initializeD3d(videoFormat, width, height, frameRate);
    impl_->initializeDecoder(videoFormat, width, height);
}

int VideoRenderer::submit(PDECODE_UNIT decodeUnit) {
    return impl_->submit(decodeUnit);
}

bool VideoRenderer::recoveryRequired() const { return impl_->recoveryRequested.load(); }

std::optional<int> VideoRenderer::recoveryDisplayIndex() const {
    const auto index = impl_->recoveryDisplayIndex.load();
    return index >= 0 ? std::optional<int>{index} : std::nullopt;
}

void VideoRenderer::setInputEnabled(bool enabled) {
    impl_->setInputEnabled(enabled);
}

void VideoRenderer::setHdrMode(bool enabled) {
    impl_->setHdrMode(enabled);
}

void VideoRenderer::resumeOverlay() { impl_->resumeOverlay(); }

void VideoRenderer::setGamepadRumble(std::uint16_t controllerNumber,
                                     std::uint16_t lowFrequency,
                                     std::uint16_t highFrequency) {
    impl_->setGamepadRumble(controllerNumber, lowFrequency, highFrequency);
}

void VideoRenderer::setGamepadTriggerRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}

void VideoRenderer::setGamepadMotionEventState(std::uint16_t controllerNumber,
                                                std::uint8_t motionType,
                                                std::uint16_t reportRateHz) {
    impl_->setGamepadMotionEventState(controllerNumber, motionType, reportRateHz);
}

void VideoRenderer::setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red,
                                  std::uint8_t green, std::uint8_t blue) {
    impl_->setGamepadLed(controllerNumber, red, green, blue);
}

}  // namespace eclipse

#elif defined(__linux__) && defined(ECLIPSE_HAS_LINUX_VIDEO)

#include "input_forwarder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>
#if defined(ECLIPSE_HAS_LIBPLACEBO)
#include <SDL_vulkan.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#endif
#if defined(ECLIPSE_HAS_VAAPI_X11)
#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_x11.h>
#endif

#if defined(ECLIPSE_HAS_VAAPI_X11) && defined(SDL_VIDEO_DRIVER_X11)
#define ECLIPSE_USE_VAAPI_X11 1
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#if defined(ECLIPSE_HAS_VAAPI_X11)
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}
namespace eclipse {
namespace {

std::string ffmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}

const AVCodecHWConfig* hardwareConfigFor(const AVCodec* decoder,
                                         AVHWDeviceType deviceType) {
    if (!decoder) return nullptr;
    for (int index = 0;; ++index) {
        const auto* config = avcodec_get_hw_config(decoder, index);
        if (!config) return nullptr;
        if (config->device_type == deviceType &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return config;
        }
    }
}

const AVCodec* hardwareDecoderFor(AVCodecID codecId, AVHWDeviceType deviceType) {
    void* iterator = nullptr;
    while (const auto* decoder = av_codec_iterate(&iterator)) {
        if (av_codec_is_decoder(decoder) && decoder->id == codecId &&
            hardwareConfigFor(decoder, deviceType)) {
            return decoder;
        }
    }
    return nullptr;
}

std::string sdlError(const char* message) {
    return std::string{message} + ": " + SDL_GetError();
}

}  // namespace

int selectVideoFormat(VideoCodec preference, int serverCodecModeSupport, bool enableHdr,
                      bool enableYuv444) {
#if !defined(ECLIPSE_HAS_LIBPLACEBO)
    if (enableHdr || enableYuv444) {
        throw std::runtime_error(
            "Linux HDR and YUV 4:4:4 require optional Vulkan/libplacebo support.");
    }
#endif
    if (preference == VideoCodec::h264 && enableHdr) {
        throw std::runtime_error("HDR requires HEVC or AV1.");
    }
    const auto available = [&](int format, int capability, AVCodecID codec) {
        return (serverCodecModeSupport & capability) != 0 && avcodec_find_decoder(codec)
                   ? format
                   : 0;
    };
    const auto h264 = [&] {
        return enableYuv444
                   ? available(VIDEO_FORMAT_H264_HIGH8_444, SCM_H264_HIGH8_444,
                               AV_CODEC_ID_H264)
                   : available(VIDEO_FORMAT_H264, SCM_H264, AV_CODEC_ID_H264);
    };
    const auto hevc = [&] {
        if (enableHdr && enableYuv444) {
            return available(VIDEO_FORMAT_H265_REXT10_444, SCM_HEVC_REXT10_444,
                             AV_CODEC_ID_HEVC);
        }
        if (enableHdr) {
            return available(VIDEO_FORMAT_H265_MAIN10, SCM_HEVC_MAIN10, AV_CODEC_ID_HEVC);
        }
        if (enableYuv444) {
            return available(VIDEO_FORMAT_H265_REXT8_444, SCM_HEVC_REXT8_444,
                             AV_CODEC_ID_HEVC);
        }
        return available(VIDEO_FORMAT_H265, SCM_HEVC, AV_CODEC_ID_HEVC);
    };
    const auto av1 = [&] {
        if (enableHdr && enableYuv444) {
            return available(VIDEO_FORMAT_AV1_HIGH10_444, SCM_AV1_HIGH10_444, AV_CODEC_ID_AV1);
        }
        if (enableHdr) {
            return available(VIDEO_FORMAT_AV1_MAIN10, SCM_AV1_MAIN10, AV_CODEC_ID_AV1);
        }
        if (enableYuv444) {
            return available(VIDEO_FORMAT_AV1_HIGH8_444, SCM_AV1_HIGH8_444, AV_CODEC_ID_AV1);
        }
        return available(VIDEO_FORMAT_AV1_MAIN8, SCM_AV1_MAIN8, AV_CODEC_ID_AV1);
    };
    if (preference == VideoCodec::automatic) {
        if (!enableHdr) {
            if (const auto format = h264()) return format;
        }
        if (const auto format = hevc()) return format;
        if (const auto format = av1()) return format;
        throw std::runtime_error("No mutually supported Linux video decoder is available.");
    }
    if (preference == VideoCodec::h264) {
        if (const auto format = h264()) return format;
        if (enableYuv444) {
            throw std::runtime_error("Host or FFmpeg does not support H.264 High 4:4:4 streaming.");
        } else {
            throw std::runtime_error("Host or FFmpeg does not support H.264 streaming.");
        }
    }
    const auto format = preference == VideoCodec::av1 ? av1() : hevc();
    if (format) return format;
    throw std::runtime_error(std::string{"Host or FFmpeg does not support requested "} +
                             (preference == VideoCodec::av1 ? "AV1" : "HEVC") +
                             (enableHdr ? " 10-bit" : "") +
                             (enableYuv444 ? " 4:4:4" : "") + " streaming.");
}

struct VideoRenderer::Impl {
    enum class FrameQueueResult {
        queued,
        decoderFallback,
        failed,
    };

    struct PendingFrame {
        int width;
        int height;
        int chromaWidth;
        int chromaHeight;
        bool fullRange;
        bool hardwareDecoded;
        std::vector<std::uint8_t> y;
        std::vector<std::uint8_t> u;
        std::vector<std::uint8_t> v;
        std::shared_ptr<AVFrame> hardwareFrame;
        std::shared_ptr<AVFrame> avFrame;
        std::chrono::steady_clock::time_point decodedAt;
    };

    struct FrameColorInfo {
        std::int64_t frameNumber;
        bool hdrActive;
        std::uint8_t colorspace;
    };

    struct GamepadRumbleCommand {
        std::uint16_t controllerNumber;
        std::uint16_t lowFrequency;
        std::uint16_t highFrequency;
    };

    struct GamepadMotionCommand {
        std::uint16_t controllerNumber;
        std::uint8_t motionType;
        std::uint16_t reportRateHz;
    };

    struct GamepadLedCommand {
        std::uint16_t controllerNumber;
        std::uint8_t red;
        std::uint8_t green;
        std::uint8_t blue;
    };

    Impl(StreamSettings streamSettings, StatusListener callback, CloseListener closeCallback,
          std::shared_ptr<StreamStatistics> streamStatistics, OverlayListener overlayCallback)
        : settings(std::move(streamSettings)),
          listener(std::move(callback)),
          closeListener(std::move(closeCallback)),
          statistics(std::move(streamStatistics)),
          overlayListener(std::move(overlayCallback)) {
        hdrModeRequested.store(LiGetCurrentHostDisplayHdrMode());
        overlayVisible = settings.showPerformanceStats;
    }

    ~Impl() {
        {
            std::scoped_lock lock{frameMutex};
            stopping = true;
        }
        frameCondition.notify_one();
        if (renderThread.joinable()) renderThread.join();
        {
            std::scoped_lock lock{frameMutex};
            pendingFrames.clear();
        }
        if (codecContext) avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDevice);
#if defined(ECLIPSE_USE_VAAPI_X11)
        if (vaapiDisplay) vaTerminate(vaapiDisplay);
        if (vaapiX11Display) XCloseDisplay(vaapiX11Display);
#endif
    }

    void notify(std::string state, std::string message) noexcept {
        try {
            if (listener) listener(std::move(state), std::move(message));
        } catch (...) {
        }
    }

    void setHdrMode(bool enabled) noexcept { hdrModeRequested.store(enabled); }

#if defined(ECLIPSE_HAS_LIBPLACEBO)
    bool surfaceSupportsHdr(VkPhysicalDevice device) const {
        const auto getSurfaceFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            placeboInstance->get_proc_addr(placeboInstance->instance,
                                            "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        if (!getSurfaceFormats) return false;
        std::uint32_t count = 0;
        if (getSurfaceFormats(device, placeboSurface, &count, nullptr) != VK_SUCCESS || count == 0) {
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (getSurfaceFormats(device, placeboSurface, &count, formats.data()) != VK_SUCCESS) {
            return false;
        }
        return std::ranges::any_of(formats, [](const auto& format) {
            return format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        });
    }

    void initializePlacebo() {
        pl_log_params logParameters = pl_log_default_params;
        logParameters.log_cb = pl_log_simple;
        logParameters.log_priv = stderr;
        logParameters.log_level = PL_LOG_WARN;
        placeboLog = pl_log_create(PL_API_VER, &logParameters);
        if (!placeboLog) throw std::runtime_error("Cannot create libplacebo log context.");

        unsigned int extensionCount = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) {
            throw std::runtime_error(sdlError("Cannot query SDL Vulkan extensions"));
        }
        std::vector<const char*> extensions(extensionCount);
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) {
            throw std::runtime_error(sdlError("Cannot read SDL Vulkan extensions"));
        }
        const char* optionalExtensions[] = {VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
        pl_vk_inst_params instanceParameters = pl_vk_inst_default_params;
        instanceParameters.get_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            SDL_Vulkan_GetVkGetInstanceProcAddr());
        instanceParameters.extensions = extensions.data();
        instanceParameters.num_extensions = static_cast<int>(extensions.size());
        instanceParameters.opt_extensions = optionalExtensions;
        instanceParameters.num_opt_extensions = 1;
        placeboInstance = pl_vk_inst_create(placeboLog, &instanceParameters);
        if (!placeboInstance) throw std::runtime_error("Cannot create Vulkan instance.");
        if (!SDL_Vulkan_CreateSurface(window, placeboInstance->instance, &placeboSurface)) {
            throw std::runtime_error(sdlError("Cannot create Vulkan window surface"));
        }

        pl_vulkan_device_params deviceParameters{};
        deviceParameters.instance = placeboInstance->instance;
        deviceParameters.get_proc_addr = placeboInstance->get_proc_addr;
        deviceParameters.surface = placeboSurface;
        const auto physicalDevice = pl_vulkan_choose_device(placeboLog, &deviceParameters);
        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("No Vulkan device can present to the selected display.");
        }
        if (settings.enableHdr && !surfaceSupportsHdr(physicalDevice)) {
            throw std::runtime_error(
                "Selected Linux display does not expose Vulkan HDR10 ST2084 output.");
        }

        pl_vulkan_params vulkanParameters = pl_vulkan_default_params;
        vulkanParameters.instance = placeboInstance->instance;
        vulkanParameters.get_proc_addr = placeboInstance->get_proc_addr;
        vulkanParameters.surface = placeboSurface;
        vulkanParameters.device = physicalDevice;
        placeboVulkan = pl_vulkan_create(placeboLog, &vulkanParameters);
        if (!placeboVulkan) throw std::runtime_error("Cannot create libplacebo Vulkan device.");

        pl_vulkan_swapchain_params swapchainParameters{};
        swapchainParameters.surface = placeboSurface;
        swapchainParameters.present_mode = settings.enableVsync ? VK_PRESENT_MODE_FIFO_KHR
                                                                : VK_PRESENT_MODE_MAILBOX_KHR;
        swapchainParameters.swapchain_depth = 1;
        swapchainParameters.disable_10bit_sdr = true;
        swapchainParameters.color_bits = settings.enableHdr ? 10 : 8;
        placeboSwapchain = pl_vulkan_create_swapchain(placeboVulkan, &swapchainParameters);
        if (!placeboSwapchain) throw std::runtime_error("Cannot create Vulkan presentation swapchain.");
        if (settings.enableHdr) {
            pl_swapchain_colorspace_hint(placeboSwapchain, &pl_color_space_hdr10);
        }
        placeboRenderer = pl_renderer_create(placeboLog, placeboVulkan->gpu);
        if (!placeboRenderer) throw std::runtime_error("Cannot create libplacebo renderer.");
        placeboActive = true;
    }

    void cleanupPlacebo() noexcept {
        if (placeboRenderer) pl_renderer_destroy(&placeboRenderer);
        if (placeboVulkan) {
            for (auto& texture : placeboTextures) {
                if (texture) pl_tex_destroy(placeboVulkan->gpu, &texture);
            }
            if (placeboOverlayTexture) {
                pl_tex_destroy(placeboVulkan->gpu, &placeboOverlayTexture);
            }
        }
        if (placeboSwapchain) pl_swapchain_destroy(&placeboSwapchain);
        if (placeboVulkan) pl_vulkan_destroy(&placeboVulkan);
        if (placeboSurface != VK_NULL_HANDLE && placeboInstance) {
            const auto destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                placeboInstance->get_proc_addr(placeboInstance->instance, "vkDestroySurfaceKHR"));
            if (destroySurface) {
                destroySurface(placeboInstance->instance, placeboSurface, nullptr);
            }
            placeboSurface = VK_NULL_HANDLE;
        }
        if (placeboInstance) pl_vk_inst_destroy(&placeboInstance);
        if (placeboLog) pl_log_destroy(&placeboLog);
        placeboActive = false;
    }
#endif

    void cleanupSdl() {
        inputForwarder.stop();
#if defined(ECLIPSE_HAS_LIBPLACEBO)
        cleanupPlacebo();
#endif
#if defined(ECLIPSE_USE_VAAPI_X11)
        if (vaapiX11Display) XSync(vaapiX11Display, False);
#endif
        if (overlayTexture) SDL_DestroyTexture(overlayTexture);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
#if SDL_VERSION_ATLEAST(2, 24, 0)
        SDL_ResetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH);
#else
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "0");
#endif
        if (previousMouseFocusClickthrough) {
            SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,
                        previousMouseFocusClickthrough->c_str());
        }
        if (yuvConversionModeSaved) SDL_SetYUVConversionMode(previousYuvConversionMode);
        if (screenSaverDisabled) SDL_EnableScreenSaver();
        if (sdlVideoInitialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    bool createSdlRenderer() {
        if (renderer) return true;
        renderer = SDL_CreateRenderer(window, -1, rendererFlags);
        if (!renderer) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (renderer && settings.enableVsync) {
                notify("connected", "Accelerated V-Sync unavailable; using software presentation.");
            }
        }
        if (!renderer) return false;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
        previousYuvConversionMode = SDL_GetYUVConversionMode();
        yuvConversionModeSaved = true;
        return true;
    }

    void initializeSdl(int width, int height, int frameRate) {
#if defined(ECLIPSE_USE_VAAPI_X11)
        XInitThreads();
#endif
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
            throw std::runtime_error(sdlError("Cannot initialize SDL video"));
        }
        sdlVideoInitialized = true;

        if (const char* previous = SDL_GetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH)) {
            previousMouseFocusClickthrough = previous;
        }
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

        const int displayCount = SDL_GetNumVideoDisplays();
        const int displayIndex = settings.displayIndex >= 0 && settings.displayIndex < displayCount
                                     ? settings.displayIndex
                                     : 0;
        currentDisplayIndex = displayIndex;
        if (displayIndex != settings.displayIndex) {
            notify("connected", "Selected display is unavailable; using primary display.");
        }
        SDL_DisplayMode desktopMode{};
        const bool hasDesktopMode = SDL_GetDesktopDisplayMode(displayIndex, &desktopMode) == 0;
        SDL_DisplayMode fullscreenMode = desktopMode;
        bool hasFullscreenMode = false;
        if (settings.displayMode == DisplayMode::fullscreen && hasDesktopMode) {
            std::vector<SDL_DisplayMode> availableModes;
            std::vector<DisplayModeSpec> modeSpecs;
            const auto modeCount = SDL_GetNumDisplayModes(displayIndex);
            for (int modeIndex = 0; modeIndex < modeCount; ++modeIndex) {
                SDL_DisplayMode mode{};
                if (SDL_GetDisplayMode(displayIndex, modeIndex, &mode) == 0) {
                    availableModes.push_back(mode);
                    modeSpecs.push_back({mode.w, mode.h, mode.refresh_rate});
                }
            }
            const auto selected = selectOptimalDisplayMode(
                modeSpecs, {desktopMode.w, desktopMode.h, desktopMode.refresh_rate}, width, height,
                frameRate);
            const auto selectedMode = std::find_if(
                availableModes.begin(), availableModes.end(), [&](const auto& mode) {
                    return mode.w == selected.width && mode.h == selected.height &&
                           mode.refresh_rate == selected.refreshRate;
                });
            if (selectedMode != availableModes.end()) {
                fullscreenMode = *selectedMode;
                hasFullscreenMode = true;
            }
        }

        Uint32 windowFlags = SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(ECLIPSE_HAS_LIBPLACEBO)
        const bool waylandSession = std::getenv("XDG_SESSION_TYPE") &&
                                    std::string_view{std::getenv("XDG_SESSION_TYPE")} == "wayland";
        const bool usePlacebo = settings.enableHdr || settings.enableYuv444 || waylandSession;
        if (usePlacebo) windowFlags |= SDL_WINDOW_VULKAN;
#endif
        int outputWidth = width;
        int outputHeight = height;
        if (settings.displayMode == DisplayMode::windowed) {
            SDL_Rect usableBounds{};
            if (SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) == 0) {
                const auto scale = std::min(
                    {1.0, static_cast<double>(std::max(usableBounds.w - 80, 640)) / width,
                     static_cast<double>(std::max(usableBounds.h - 120, 360)) / height});
                outputWidth = std::max(static_cast<int>(width * scale), 640);
                outputHeight = std::max(static_cast<int>(height * scale), 360);
            }
            windowFlags |= SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        } else if (settings.displayMode == DisplayMode::borderless) {
            if (hasDesktopMode) {
                outputWidth = desktopMode.w;
                outputHeight = desktopMode.h;
            }
            windowFlags |= SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP;
        } else {
            if (hasFullscreenMode) {
                outputWidth = fullscreenMode.w;
                outputHeight = fullscreenMode.h;
            } else if (hasDesktopMode) {
                outputWidth = desktopMode.w;
                outputHeight = desktopMode.h;
            }
        }

        const int windowPosition = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
        window = SDL_CreateWindow("Eclipse Stream", windowPosition, windowPosition, outputWidth,
                                  outputHeight, windowFlags);
        if (!window) throw std::runtime_error(sdlError("Cannot create SDL stream window"));
        int presentationRefreshRate = hasDesktopMode ? desktopMode.refresh_rate : 0;
        if (settings.displayMode == DisplayMode::fullscreen) {
            const bool modeApplied = !hasFullscreenMode ||
                                     SDL_SetWindowDisplayMode(window, &fullscreenMode) == 0;
            if (!modeApplied) {
                notify("connected", sdlError("Cannot apply cadence-matched display mode"));
            }
            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) < 0) {
                notify("connected",
                       sdlError("Exclusive fullscreen unavailable; using borderless fullscreen"));
                static_cast<void>(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP));
            }
            SDL_ShowWindow(window);
            SDL_DisplayMode activeMode{};
            if (SDL_GetCurrentDisplayMode(displayIndex, &activeMode) == 0) {
                presentationRefreshRate = activeMode.refresh_rate;
            }
        }

#if defined(ECLIPSE_HAS_LIBPLACEBO)
        if (usePlacebo) initializePlacebo();
#endif

#if defined(ECLIPSE_USE_VAAPI_X11)
        if (!placeboActive && !settings.showPerformanceStats &&
            (!std::getenv("XDG_SESSION_TYPE") ||
             std::string_view{std::getenv("XDG_SESSION_TYPE")} != "wayland")) {
            SDL_SysWMinfo windowInfo{};
            SDL_VERSION(&windowInfo.version);
            if (SDL_GetWindowWMInfo(window, &windowInfo) == SDL_TRUE &&
                windowInfo.subsystem == SDL_SYSWM_X11) {
                vaapiX11Window = windowInfo.info.x11.window;
                vaapiX11Display = XOpenDisplay(DisplayString(windowInfo.info.x11.display));
                if (vaapiX11Display) {
                    vaapiDisplay = vaGetDisplay(vaapiX11Display);
                    int major = 0;
                    int minor = 0;
                    const auto initializeStatus =
                        vaapiDisplay ? vaInitialize(vaapiDisplay, &major, &minor)
                                     : VA_STATUS_ERROR_INVALID_DISPLAY;
                    std::vector<VAEntrypoint> entrypoints(
                        vaapiDisplay ? static_cast<std::size_t>(vaMaxNumEntrypoints(vaapiDisplay))
                                     : 0);
                    int entrypointCount = 0;
                    const bool videoProcessing =
                        initializeStatus == VA_STATUS_SUCCESS && !entrypoints.empty() &&
                        vaQueryConfigEntrypoints(vaapiDisplay, VAProfileNone, entrypoints.data(),
                                                 &entrypointCount) == VA_STATUS_SUCCESS &&
                        std::find(entrypoints.begin(),
                                  entrypoints.begin() + entrypointCount,
                                  VAEntrypointVideoProc) !=
                            entrypoints.begin() + entrypointCount;
                    if (!videoProcessing) {
                        if (vaapiDisplay) vaTerminate(vaapiDisplay);
                        vaapiDisplay = nullptr;
                        XCloseDisplay(vaapiX11Display);
                        vaapiX11Display = nullptr;
                    } else {
                        directPresentationEnabled.store(true);
                        XSetWindowBackground(vaapiX11Display, vaapiX11Window, 0);
                        XClearWindow(vaapiX11Display, vaapiX11Window);
                        XFlush(vaapiX11Display);
                    }
                }
            }
        }
#endif

        const bool presentWithVsync =
            settings.enableVsync &&
            (presentationRefreshRate <= 1 || frameRate <= presentationRefreshRate + 1);
        if (settings.enableVsync && !presentWithVsync) {
            notify("connected", "V-Sync bypassed because stream FPS exceeds display refresh rate.");
        }
        rendererFlags = SDL_RENDERER_ACCELERATED |
                        (presentWithVsync ? SDL_RENDERER_PRESENTVSYNC : 0);
        if (!placeboActive && !directPresentationEnabled.load() && !createSdlRenderer()) {
            throw std::runtime_error(sdlError("Cannot create SDL renderer"));
        }
        inputForwarder.start(window, settings.input, width, height, frameRate,
                              [this] { togglePerformanceOverlay(); },
                              [this] { return toggleFullscreen(); },
                              [this] { requestOverlay(); });

        if (settings.keepAwake) {
            SDL_DisableScreenSaver();
            screenSaverDisabled = true;
        }
        nextOverlayUpdate = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }

    static AVPixelFormat selectPixelFormat(AVCodecContext* context,
                                            const AVPixelFormat* formats) {
        auto* self = static_cast<Impl*>(context->opaque);
        for (auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == self->hardwarePixelFormat) {
                self->hardwareDecodeActive.store(true);
                return *format;
            }
        }
        self->hardwareDecodeActive.store(false);
        return AV_PIX_FMT_NONE;
    }

    bool openDecoder(bool tryHardware) {
        if (codecContext) avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDevice);
        hardwarePixelFormat = AV_PIX_FMT_NONE;
        hardwareDecoderConfigured = false;
        hardwareDecodeActive.store(false);
        vaapiX11Device.store(false);
        frameColors.clear();
        if (!tryHardware) directPresentationEnabled.store(false);

        codecContext = avcodec_alloc_context3(decoderCodec);
        if (!codecContext) throw std::runtime_error("Cannot allocate FFmpeg decoder context.");
        codecContext->width = decoderWidth;
        codecContext->height = decoderHeight;
        codecContext->thread_count = tryHardware ? 1 : 0;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->flags2 |= AV_CODEC_FLAG2_FAST;

        if (tryHardware) {
            const auto* hardwareConfig =
                hardwareConfigFor(decoderCodec, AV_HWDEVICE_TYPE_VAAPI);
            if (!hardwareConfig) {
                avcodec_free_context(&codecContext);
                return false;
            }
#if defined(ECLIPSE_USE_VAAPI_X11)
            if (directPresentationEnabled.load() && vaapiDisplay) {
                hardwareDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
                if (hardwareDevice) {
                    auto* device = reinterpret_cast<AVHWDeviceContext*>(hardwareDevice->data);
                    auto* vaapi = static_cast<AVVAAPIDeviceContext*>(device->hwctx);
                    vaapi->display = vaapiDisplay;
                    if (av_hwdevice_ctx_init(hardwareDevice) >= 0) {
                        vaapiX11Device.store(true);
                    } else {
                        av_buffer_unref(&hardwareDevice);
                    }
                }
            }
#endif
            if (!hardwareDevice &&
                av_hwdevice_ctx_create(&hardwareDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr,
                                       0) < 0) {
                avcodec_free_context(&codecContext);
                av_buffer_unref(&hardwareDevice);
                directPresentationEnabled.store(false);
                return false;
            }
            if (!vaapiX11Device.load()) directPresentationEnabled.store(false);
            hardwarePixelFormat = hardwareConfig->pix_fmt;
            codecContext->opaque = this;
            codecContext->get_format = selectPixelFormat;
            codecContext->hw_device_ctx = av_buffer_ref(hardwareDevice);
            if (!codecContext->hw_device_ctx) {
                avcodec_free_context(&codecContext);
                av_buffer_unref(&hardwareDevice);
                return false;
            }
        }

        const auto result = avcodec_open2(codecContext, decoderCodec, nullptr);
        if (result < 0) {
            avcodec_free_context(&codecContext);
            av_buffer_unref(&hardwareDevice);
            if (tryHardware) return false;
            throw std::runtime_error(std::string{"Cannot open FFmpeg "} +
                                     videoCodecName(activeVideoFormat) + " decoder: " +
                                     ffmpegError(result));
        }
        hardwareDecoderConfigured = tryHardware;
        return true;
    }

    void initializeDecoder(int videoFormat, int width, int height) {
        const AVCodecID codecId = isAv1VideoFormat(videoFormat)   ? AV_CODEC_ID_AV1
                                   : isHevcVideoFormat(videoFormat) ? AV_CODEC_ID_HEVC
                                                                     : AV_CODEC_ID_H264;
        activeVideoFormat = videoFormat;
        activeCodecId = codecId;
        decoderWidth = width;
        decoderHeight = height;
        bool hardwarePresentationAvailable = false;
#if defined(ECLIPSE_HAS_LIBPLACEBO)
        hardwarePresentationAvailable = placeboActive;
#endif
#if defined(ECLIPSE_USE_VAAPI_X11)
        hardwarePresentationAvailable =
            hardwarePresentationAvailable || directPresentationEnabled.load();
#endif
        if (hardwarePresentationAvailable) {
            decoderCodec = hardwareDecoderFor(codecId, AV_HWDEVICE_TYPE_VAAPI);
            if (decoderCodec && openDecoder(true)) return;
        }
        decoderCodec = avcodec_find_decoder(codecId);
        if (!decoderCodec) {
            throw std::runtime_error(std::string{"FFmpeg "} + videoCodecName(videoFormat) +
                                     " decoder is unavailable.");
        }
        openDecoder(false);
    }

    bool fallBackToSoftwareDecoder(std::string reason) {
        if (!hardwareDecoderConfigured) return false;
        try {
            decoderCodec = avcodec_find_decoder(activeCodecId);
            if (!decoderCodec) throw std::runtime_error("Software decoder is unavailable.");
            openDecoder(false);
            referenceFrameAccepted = false;
            hardwarePresentationFallbackRequested.store(false);
            {
                std::scoped_lock lock{frameMutex};
                pendingFrames.clear();
            }
            notify("connected", std::move(reason) + " Falling back to software decode.");
            return true;
        } catch (const std::exception& exception) {
            recoveryRequested.store(true);
            notify("connected", std::string{"Software decoder fallback failed: "} +
                                    exception.what());
            return false;
        }
    }

    void finishInitialization(std::string error = {}) {
        {
            std::scoped_lock lock{initializationMutex};
            initializationError = std::move(error);
            initializationComplete = true;
        }
        initializationCondition.notify_one();
    }

    void renderLoop(int width, int height, int frameRate) {
        try {
            initializeSdl(width, height, frameRate);
            finishInitialization();
            while (true) {
                std::optional<PendingFrame> frame;
                std::array<std::optional<GamepadRumbleCommand>, 16> rumble;
                std::array<std::optional<GamepadRumbleCommand>, 16> triggerRumble;
                std::array<std::optional<GamepadMotionCommand>, 32> motion;
                std::array<std::optional<GamepadLedCommand>, 16> led;
                {
                    std::unique_lock lock{frameMutex};
                    frameCondition.wait_for(lock, std::chrono::milliseconds{8}, [this] {
                        return stopping || !pendingFrames.empty() ||
                               inputRequestGeneration != inputAppliedGeneration ||
                               overlayResumeRequested ||
                               std::ranges::any_of(pendingRumble,
                                                   [](const auto& value) { return value.has_value(); }) ||
                               std::ranges::any_of(pendingTriggerRumble,
                                                    [](const auto& value) { return value.has_value(); }) ||
                               std::ranges::any_of(pendingMotion,
                                                   [](const auto& value) { return value.has_value(); }) ||
                               std::ranges::any_of(pendingLed,
                                                   [](const auto& value) { return value.has_value(); });
                    });
                    if (stopping) break;
                    if (!pendingFrames.empty()) {
                        frame = std::move(pendingFrames.front());
                        pendingFrames.pop_front();
                    }
                    rumble.swap(pendingRumble);
                    triggerRumble.swap(pendingTriggerRumble);
                    motion.swap(pendingMotion);
                    led.swap(pendingLed);
                }
                applyInputState();
                applyOverlayResume();
                for (const auto& command : rumble) {
                    if (!command) continue;
                    inputForwarder.setGamepadRumble(command->controllerNumber,
                                                    command->lowFrequency,
                                                    command->highFrequency);
                }
                for (const auto& command : triggerRumble) {
                    if (!command) continue;
                    inputForwarder.setGamepadTriggerRumble(
                        command->controllerNumber, command->lowFrequency,
                        command->highFrequency);
                }
                for (const auto& command : motion) {
                    if (!command) continue;
                    inputForwarder.setGamepadMotionEventState(
                        command->controllerNumber, command->motionType, command->reportRateHz);
                }
                for (const auto& command : led) {
                    if (!command) continue;
                    inputForwarder.setGamepadLed(command->controllerNumber, command->red,
                                                 command->green, command->blue);
                }
                inputForwarder.updateGamepads();
                processEvents();
                if (frame) {
                    if (!render(*frame)) {
                        recoveryRequested.store(true);
                        if (!presentationErrorReported) {
                            presentationErrorReported = true;
                            notify("connected", sdlError("SDL video presentation failed"));
                        }
                    } else {
                        presentationErrorReported = false;
                        if (!presentationReported) {
                            presentationReported = true;
                            std::string presentation;
                            if (placeboActive) {
                                presentation = hardwareDecodeActive.load() ? " VAAPI decode with "
                                                                           : " software decode with ";
                                 presentation += settings.enableHdr && settings.enableYuv444
                                                     ? "Vulkan HDR10 4:4:4 presentation active."
                                                 : settings.enableHdr
                                                     ? "Vulkan HDR10 presentation active."
                                                : settings.enableYuv444
                                                    ? "Vulkan 4:4:4 presentation active."
                                                    : "Vulkan presentation active.";
                            } else if (directPresentationUsed.load()) {
                                presentation = " VAAPI zero-copy X11 presentation active.";
                            } else if (hardwareDecodeActive.load()) {
                                presentation = " VAAPI decode with SDL presentation active.";
                            } else {
                                presentation = " software decode and SDL presentation active.";
                            }
                            notify("rendering", std::string{"FFmpeg "} +
                                                    videoCodecName(activeVideoFormat) + presentation);
                        }
                    }
                }
            }
        } catch (const std::exception& exception) {
            if (!initializationComplete) {
                finishInitialization(exception.what());
            } else {
                recoveryRequested.store(true);
                notify("connected", exception.what());
            }
        }
        cleanupSdl();
        {
            std::scoped_lock lock{frameMutex};
            renderThreadExited = true;
        }
        inputCondition.notify_all();
    }

    void initialize(int videoFormat, int width, int height, int frameRate) {
        renderThread =
            std::thread([this, width, height, frameRate] { renderLoop(width, height, frameRate); });
        std::unique_lock lock{initializationMutex};
        initializationCondition.wait(lock, [this] { return initializationComplete; });
        if (!initializationError.empty()) throw std::runtime_error(initializationError);
        lock.unlock();
        initializeDecoder(videoFormat, width, height);
    }

    void applyInputState() {
        bool enabled = false;
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock{frameMutex};
            if (inputRequestGeneration == inputAppliedGeneration) return;
            enabled = inputEnabled;
            generation = inputRequestGeneration;
        }
        inputForwarder.setEnabled(enabled);
        {
            std::scoped_lock lock{frameMutex};
            inputAppliedGeneration = generation;
        }
        inputCondition.notify_all();
    }

    void setInputEnabled(bool enabled) {
        std::unique_lock lock{frameMutex};
        if (stopping || renderThreadExited) return;
        inputEnabled = enabled;
        const auto generation = ++inputRequestGeneration;
        frameCondition.notify_one();
        inputCondition.wait(lock, [this, generation] {
            return stopping || renderThreadExited || inputAppliedGeneration >= generation;
        });
    }

    void requestOverlay() {
        if (!overlayListener || !window) return;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &width, &height);
        float dpi = 96.0F;
        const int display = SDL_GetWindowDisplayIndex(window);
        if (display >= 0) {
            static_cast<void>(SDL_GetDisplayDPI(display, &dpi, nullptr, nullptr));
        }
        const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
        const bool wayland = waylandDisplay && *waylandDisplay;
        const Uint32 flags = SDL_GetWindowFlags(window);
        const bool fullscreen =
            (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
        overlayListener({
            .x = x,
            .y = y,
            .width = std::max(width, 1),
            .height = std::max(height, 1),
            .scaleFactor = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0,
            .wayland = wayland,
            .fullscreen = fullscreen,
        });
        streamOverlayActive = true;
    }

    void applyOverlayResume() {
        {
            std::scoped_lock lock{frameMutex};
            if (!overlayResumeRequested) return;
            overlayResumeRequested = false;
        }
        streamOverlayActive = false;
        SDL_RaiseWindow(window);
        static_cast<void>(SDL_SetWindowInputFocus(window));
        inputForwarder.resumeAfterOverlay();
    }

    void resumeOverlay() {
        {
            std::scoped_lock lock{frameMutex};
            if (stopping || renderThreadExited) return;
            overlayResumeRequested = true;
        }
        frameCondition.notify_one();
    }

    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                          std::uint16_t highFrequency) {
        {
            std::scoped_lock lock{frameMutex};
            if (stopping || renderThreadExited || controllerNumber >= pendingRumble.size()) return;
            pendingRumble[controllerNumber] = {controllerNumber, lowFrequency, highFrequency};
        }
        frameCondition.notify_one();
    }

    void setGamepadTriggerRumble(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                 std::uint16_t rightTrigger) {
        {
            std::scoped_lock lock{frameMutex};
            if (stopping || renderThreadExited || controllerNumber >= pendingTriggerRumble.size()) {
                return;
            }
            pendingTriggerRumble[controllerNumber] = {controllerNumber, leftTrigger, rightTrigger};
        }
        frameCondition.notify_one();
    }

    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz) {
        if (controllerNumber >= 16 || motionType < LI_MOTION_TYPE_ACCEL ||
            motionType > LI_MOTION_TYPE_GYRO) {
            return;
        }
        {
            std::scoped_lock lock{frameMutex};
            if (stopping || renderThreadExited) return;
            const auto index = controllerNumber * 2 + motionType - 1;
            pendingMotion[index] = {controllerNumber, motionType, reportRateHz};
        }
        frameCondition.notify_one();
    }

    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue) {
        {
            std::scoped_lock lock{frameMutex};
            if (stopping || renderThreadExited || controllerNumber >= pendingLed.size()) return;
            pendingLed[controllerNumber] = {controllerNumber, red, green, blue};
        }
        frameCondition.notify_one();
    }

    void processEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            inputForwarder.handleEvent(event);
            if (event.type == SDL_RENDER_DEVICE_RESET || event.type == SDL_RENDER_TARGETS_RESET) {
                recoveryRequested.store(true);
            } else if (event.type == SDL_DISPLAYEVENT &&
                       event.display.event == SDL_DISPLAYEVENT_DISCONNECTED) {
                requestDisplayRecovery(true);
            } else if (event.type == SDL_WINDOWEVENT &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                if (streamOverlayActive && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    requestOverlay();
                }
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    requestDisplayRecovery();
                }
#else
                if (event.window.event == SDL_WINDOWEVENT_MOVED) requestDisplayRecovery();
#endif
            }
            if (event.type != SDL_QUIT &&
                !(event.type == SDL_WINDOWEVENT &&
                  event.window.event == SDL_WINDOWEVENT_CLOSE &&
                  event.window.windowID == SDL_GetWindowID(window))) {
                continue;
            }
            inputForwarder.stop();
            SDL_HideWindow(window);
            if (!windowHidden && closeListener) {
                closeListener();
            }
            windowHidden = true;
        }
    }

    void togglePerformanceOverlay() {
        const bool visible = !overlayVisible;
        if (visible && directPresentationEnabled.exchange(false)) {
            hardwarePresentationFallbackRequested.store(true);
            if (!createSdlRenderer()) {
                notify("rendering", "Performance overlay could not enable composited presentation.");
                return;
            }
        }
        overlayVisible = visible;
    }

    bool toggleFullscreen() {
        const auto flags = SDL_GetWindowFlags(window);
        const bool fullscreen = (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
        if (SDL_SetWindowFullscreen(window, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) < 0) {
            notify("rendering", sdlError("Cannot toggle fullscreen mode"));
            return fullscreen;
        }
        settings.input.fullscreen = !fullscreen;
        return !fullscreen;
    }

    void requestDisplayRecovery(bool force = false) {
        const int displayIndex = SDL_GetWindowDisplayIndex(window);
        const int nextDisplay = displayIndex >= 0 ? displayIndex : 0;
        if (!force && nextDisplay == currentDisplayIndex && displayIndex >= 0) return;
        recoveryDisplayIndex.store(nextDisplay);
        recoveryRequested.store(true);
    }

    void enqueueFrame(PendingFrame frame) {
        constexpr std::size_t maximumQueuedFrames = 3;
        frame.decodedAt = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{frameMutex};
            if (pendingFrames.size() == maximumQueuedFrames) {
                pendingFrames.pop_front();
                if (statistics) statistics->recordQueueDrop();
            }
            pendingFrames.push_back(std::move(frame));
        }
        frameCondition.notify_one();
    }

    void prepareFrameColor(AVFrame* frame, bool hdrActive, std::uint8_t colorspace) const {
        if (frame->color_range == AVCOL_RANGE_UNSPECIFIED) frame->color_range = AVCOL_RANGE_MPEG;
        const bool hdr = isHdrVideoFormat(activeVideoFormat) && hdrActive;
        if (frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
            switch (colorspace) {
                case COLORSPACE_REC_601: frame->colorspace = AVCOL_SPC_SMPTE170M; break;
                case COLORSPACE_REC_2020: frame->colorspace = AVCOL_SPC_BT2020_NCL; break;
                case COLORSPACE_REC_709:
                default: frame->colorspace = AVCOL_SPC_BT709; break;
            }
        }
        if (frame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
            frame->color_primaries = hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
        }
        if (frame->color_trc == AVCOL_TRC_UNSPECIFIED) {
            frame->color_trc = hdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
        }
        if (hdr) {
            frame->colorspace = AVCOL_SPC_BT2020_NCL;
            frame->color_primaries = AVCOL_PRI_BT2020;
            frame->color_trc = AVCOL_TRC_SMPTE2084;
        }
        if (!hdr) return;

        SS_HDR_METADATA metadata{};
        if (!LiGetHdrMetadata(&metadata)) return;
        if (!av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
            if (auto* mastering = av_mastering_display_metadata_create_side_data(frame)) {
                for (int index = 0; index < 3; ++index) {
                    mastering->display_primaries[index][0] =
                        av_make_q(metadata.displayPrimaries[index].x, 50000);
                    mastering->display_primaries[index][1] =
                        av_make_q(metadata.displayPrimaries[index].y, 50000);
                }
                mastering->white_point[0] = av_make_q(metadata.whitePoint.x, 50000);
                mastering->white_point[1] = av_make_q(metadata.whitePoint.y, 50000);
                mastering->min_luminance = av_make_q(metadata.minDisplayLuminance, 10000);
                mastering->max_luminance = av_make_q(metadata.maxDisplayLuminance, 1);
                mastering->has_luminance = metadata.maxDisplayLuminance != 0;
                mastering->has_primaries = metadata.displayPrimaries[0].x != 0;
            }
        }
        if (!av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) &&
            (metadata.maxContentLightLevel != 0 || metadata.maxFrameAverageLightLevel != 0)) {
            if (auto* light = av_content_light_metadata_create_side_data(frame)) {
                light->MaxCLL = metadata.maxContentLightLevel;
                light->MaxFALL = metadata.maxFrameAverageLightLevel;
            }
        }
    }

    std::optional<PendingFrame> copyCpuFrame(const AVFrame* frame) {
        const auto* descriptor =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (hardwareDecoderConfigured || !descriptor ||
            (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
            return std::nullopt;
        }
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
            if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_NV21) {
                return std::nullopt;
            }
        }
        PendingFrame pending{
            .width = frame->width,
            .height = frame->height,
            .chromaWidth = (frame->width + 1) / 2,
            .chromaHeight = (frame->height + 1) / 2,
            .fullRange = frame->format == AV_PIX_FMT_YUVJ420P ||
                         frame->color_range == AVCOL_RANGE_JPEG,
            .hardwareDecoded = false,
            .y = std::vector<std::uint8_t>(static_cast<std::size_t>(frame->width) * frame->height),
            .u = std::vector<std::uint8_t>(static_cast<std::size_t>((frame->width + 1) / 2) *
                                            ((frame->height + 1) / 2)),
            .v = std::vector<std::uint8_t>(static_cast<std::size_t>((frame->width + 1) / 2) *
                                             ((frame->height + 1) / 2)),
            .hardwareFrame = {},
            .avFrame = {},
            .decodedAt = {},
        };
        const auto copyPlane = [](std::vector<std::uint8_t>& destination,
                                  const std::uint8_t* source, int sourcePitch, int width,
                                  int height) {
            for (int row = 0; row < height; ++row) {
                std::memcpy(destination.data() + static_cast<std::size_t>(row) * width,
                            source + static_cast<std::ptrdiff_t>(row) * sourcePitch,
                            static_cast<std::size_t>(width));
            }
        };
        copyPlane(pending.y, frame->data[0], frame->linesize[0], pending.width, pending.height);
        if (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_NV21) {
            const bool nv21 = frame->format == AV_PIX_FMT_NV21;
            for (int row = 0; row < pending.chromaHeight; ++row) {
                const auto* source = frame->data[1] +
                                     static_cast<std::ptrdiff_t>(row) * frame->linesize[1];
                for (int column = 0; column < pending.chromaWidth; ++column) {
                    const auto destination = static_cast<std::size_t>(row) *
                                                 pending.chromaWidth +
                                             column;
                    pending.u[destination] = source[column * 2 + (nv21 ? 1 : 0)];
                    pending.v[destination] = source[column * 2 + (nv21 ? 0 : 1)];
                }
            }
        } else {
            copyPlane(pending.u, frame->data[1], frame->linesize[1], pending.chromaWidth,
                      pending.chromaHeight);
            copyPlane(pending.v, frame->data[2], frame->linesize[2], pending.chromaWidth,
                      pending.chromaHeight);
        }
        return pending;
    }

    FrameQueueResult queueFrame(const AVFrame* frame) {
        if (hardwarePresentationFallbackRequested.exchange(false) &&
            hardwareDecoderConfigured) {
            return fallBackToSoftwareDecoder("GPU presentation of VAAPI frames failed.")
                       ? FrameQueueResult::decoderFallback
                       : FrameQueueResult::failed;
        }

        const auto* pixelFormat = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool hardwareFrame =
            pixelFormat && (pixelFormat->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
        if (hardwareDecoderConfigured &&
            (frame->format != hardwarePixelFormat || !hardwareFrame)) {
            return fallBackToSoftwareDecoder("VAAPI decoder returned a CPU frame.")
                       ? FrameQueueResult::decoderFallback
                       : FrameQueueResult::failed;
        }

#if defined(ECLIPSE_HAS_LIBPLACEBO)
        if (placeboActive) {
            AVFrame* ownedFrame = av_frame_clone(frame);
            if (!ownedFrame) return FrameQueueResult::failed;
            enqueueFrame(PendingFrame{
                .width = ownedFrame->width,
                .height = ownedFrame->height,
                .chromaWidth = 0,
                .chromaHeight = 0,
                .fullRange = ownedFrame->color_range == AVCOL_RANGE_JPEG,
                .hardwareDecoded = hardwareFrame,
                .y = {},
                .u = {},
                .v = {},
                .hardwareFrame = {},
                .avFrame = std::shared_ptr<AVFrame>{ownedFrame, [](AVFrame* value) {
                    av_frame_free(&value);
                }},
                .decodedAt = {},
            });
            return FrameQueueResult::queued;
        }
#endif
        if (hardwareDecoderConfigured) {
#if defined(ECLIPSE_USE_VAAPI_X11)
            if (!directPresentationEnabled.load()) {
                return fallBackToSoftwareDecoder("No GPU presentation path accepts VAAPI frames.")
                           ? FrameQueueResult::decoderFallback
                           : FrameQueueResult::failed;
            }
            AVFrame* clone = av_frame_clone(frame);
            if (!clone) return FrameQueueResult::failed;
            enqueueFrame(PendingFrame{
                .width = frame->width,
                .height = frame->height,
                .chromaWidth = 0,
                .chromaHeight = 0,
                .fullRange = frame->color_range == AVCOL_RANGE_JPEG,
                .hardwareDecoded = true,
                .y = {},
                .u = {},
                .v = {},
                .hardwareFrame = std::shared_ptr<AVFrame>{clone, [](AVFrame* value) {
                    av_frame_free(&value);
                }},
                .avFrame = {},
                .decodedAt = {},
            });
            return FrameQueueResult::queued;
#else
            return fallBackToSoftwareDecoder("No GPU presentation path accepts VAAPI frames.")
                       ? FrameQueueResult::decoderFallback
                       : FrameQueueResult::failed;
#endif
        }
        if (hardwareFrame) {
            notify("connected", "Software decoder returned an unexpected hardware frame.");
            return FrameQueueResult::failed;
        }

        auto pending = copyCpuFrame(frame);
        if (!pending) {
            recoveryRequested.store(true);
            notify("connected", "Linux renderer received unsupported FFmpeg pixel format " +
                                    std::to_string(frame->format) + ".");
            return FrameQueueResult::failed;
        }
        enqueueFrame(std::move(*pending));
        return FrameQueueResult::queued;
    }

    bool drawPerformanceOverlay() {
        if (!statistics || !overlayVisible || overlayDisabled) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextOverlayUpdate) {
            const auto sample = statistics->latest();
            if (!sample) return true;
            const auto text = formatStreamStatistics(sample->statistics, decoderWidth,
                                                       decoderHeight,
                                                       videoCodecName(activeVideoFormat));
            const auto bitmap = rasterizePerformanceOverlay(text);
            if (overlayTexture) SDL_DestroyTexture(overlayTexture);
            overlayTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_STATIC, bitmap.width,
                                               bitmap.height);
            if (!overlayTexture || SDL_SetTextureBlendMode(overlayTexture, SDL_BLENDMODE_BLEND) < 0 ||
                SDL_UpdateTexture(overlayTexture, nullptr, bitmap.rgba.data(), bitmap.width * 4) < 0) {
                if (overlayTexture) SDL_DestroyTexture(overlayTexture);
                overlayTexture = nullptr;
                overlayDisabled = true;
                notify("rendering", "Performance overlay was disabled after an SDL error.");
                return true;
            }
            overlayWidth = bitmap.width;
            overlayHeight = bitmap.height;
            nextOverlayUpdate = now + std::chrono::seconds{1};
        }
        if (!overlayTexture) return true;
        SDL_Rect destination{10, 10, overlayWidth, overlayHeight};
        if (SDL_RenderCopy(renderer, overlayTexture, nullptr, &destination) < 0) {
            SDL_DestroyTexture(overlayTexture);
            overlayTexture = nullptr;
            overlayDisabled = true;
            notify("rendering", "Performance overlay was disabled after an SDL error.");
        }
        return true;
    }

#if defined(ECLIPSE_USE_VAAPI_X11)
    bool renderVaapiSurface(const PendingFrame& frame) {
        if (!frame.hardwareFrame || !vaapiDisplay || !vaapiX11Display || !vaapiX11Window) {
            return false;
        }
        const auto renderStarted = std::chrono::steady_clock::now();
        const auto surface =
            static_cast<VASurfaceID>(reinterpret_cast<std::uintptr_t>(frame.hardwareFrame->data[3]));
        if (surface == VA_INVALID_SURFACE || vaSyncSurface(vaapiDisplay, surface) != VA_STATUS_SUCCESS) {
            return false;
        }
        int outputWidth = 0;
        int outputHeight = 0;
        SDL_GetWindowSize(window, &outputWidth, &outputHeight);
        if (outputWidth <= 0 || outputHeight <= 0) return true;
        const double inputAspect = static_cast<double>(frame.width) / frame.height;
        const double outputAspect = static_cast<double>(outputWidth) / outputHeight;
        SDL_Rect destination{0, 0, outputWidth, outputHeight};
        if (inputAspect > outputAspect) {
            destination.h = std::max(static_cast<int>(outputWidth / inputAspect), 1);
            destination.y = (outputHeight - destination.h) / 2;
        } else {
            destination.w = std::max(static_cast<int>(outputHeight * inputAspect), 1);
            destination.x = (outputWidth - destination.w) / 2;
        }
        XClearWindow(vaapiX11Display, vaapiX11Window);
        const auto status = vaPutSurface(
            vaapiDisplay, surface, vaapiX11Window, 0, 0,
            static_cast<unsigned short>(std::min(frame.width, 65535)),
            static_cast<unsigned short>(std::min(frame.height, 65535)),
            static_cast<short>(destination.x), static_cast<short>(destination.y),
            static_cast<unsigned short>(std::min(destination.w, 65535)),
            static_cast<unsigned short>(std::min(destination.h, 65535)), nullptr, 0,
            VA_FRAME_PICTURE | VA_SRC_BT709);
        XFlush(vaapiX11Display);
        if (status != VA_STATUS_SUCCESS) return false;
        directPresentationUsed.store(true);
        if (statistics) {
            statistics->recordPresented(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - renderStarted)
                                               .count()),
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               renderStarted - frame.decodedAt)
                                               .count()));
        }
        return true;
    }
#endif

#if defined(ECLIPSE_HAS_LIBPLACEBO)
    void updatePlaceboOverlay(std::chrono::steady_clock::time_point now) {
        if (!statistics || !overlayVisible || overlayDisabled ||
            (placeboOverlayTexture && now < nextOverlayUpdate)) {
            return;
        }

        const auto sample = statistics->latest();
        if (!sample) return;
        const auto text = formatStreamStatistics(sample->statistics, decoderWidth,
                                                   decoderHeight,
                                                   videoCodecName(activeVideoFormat));
        auto bitmap = rasterizePerformanceOverlay(text);
        const auto format = pl_find_named_fmt(placeboVulkan->gpu, "rgba8");
        pl_tex_params textureParameters{};
        textureParameters.w = bitmap.width;
        textureParameters.h = bitmap.height;
        textureParameters.format = format;
        textureParameters.sampleable = true;
        textureParameters.host_writable = true;
        textureParameters.debug_tag = PL_DEBUG_TAG;
        if (!format || !pl_tex_recreate(placeboVulkan->gpu, &placeboOverlayTexture,
                                        &textureParameters)) {
            overlayDisabled = true;
            notify("rendering", "Performance overlay was disabled after a Vulkan texture error.");
            return;
        }

        pl_tex_transfer_params transferParameters{};
        transferParameters.tex = placeboOverlayTexture;
        transferParameters.row_pitch = static_cast<std::size_t>(bitmap.width) * 4;
        transferParameters.ptr = bitmap.rgba.data();
        if (!pl_tex_upload(placeboVulkan->gpu, &transferParameters)) {
            pl_tex_destroy(placeboVulkan->gpu, &placeboOverlayTexture);
            overlayDisabled = true;
            notify("rendering", "Performance overlay was disabled after a Vulkan upload error.");
            return;
        }

        overlayWidth = bitmap.width;
        overlayHeight = bitmap.height;
        nextOverlayUpdate = now + std::chrono::seconds{1};
    }

    bool renderPlacebo(const AVFrame* frame) {
        if (!frame || !placeboActive || !placeboVulkan || !placeboSwapchain || !placeboRenderer ||
            pl_gpu_is_failed(placeboVulkan->gpu)) {
            return false;
        }

        pl_frame mappedFrame{};
        pl_avframe_params mapParameters{};
        mapParameters.frame = frame;
        mapParameters.tex = placeboTextures.data();
        if (!pl_map_avframe_ex(placeboVulkan->gpu, &mappedFrame, &mapParameters)) return false;
        pl_swapchain_colorspace_hint(placeboSwapchain, &mappedFrame.color);

        int outputWidth = 0;
        int outputHeight = 0;
        SDL_Vulkan_GetDrawableSize(window, &outputWidth, &outputHeight);
        if (outputWidth <= 0 || outputHeight <= 0) {
            pl_unmap_avframe(placeboVulkan->gpu, &mappedFrame);
            return true;
        }
        if (!pl_swapchain_resize(placeboSwapchain, &outputWidth, &outputHeight)) {
            pl_unmap_avframe(placeboVulkan->gpu, &mappedFrame);
            return false;
        }

        pl_swapchain_frame swapchainFrame{};
        if (!pl_swapchain_start_frame(placeboSwapchain, &swapchainFrame)) {
            pl_unmap_avframe(placeboVulkan->gpu, &mappedFrame);
            return true;
        }
        const bool hdrFrame = mappedFrame.color.transfer == PL_COLOR_TRC_PQ;
        const auto targetDepth = std::max(swapchainFrame.color_repr.bits.sample_depth,
                                          swapchainFrame.color_repr.bits.color_depth);
        if (hdrFrame && (swapchainFrame.color_space.transfer != PL_COLOR_TRC_PQ ||
                         targetDepth < 10)) {
            static_cast<void>(pl_swapchain_submit_frame(placeboSwapchain));
            pl_unmap_avframe(placeboVulkan->gpu, &mappedFrame);
            return false;
        }
        pl_frame targetFrame{};
        pl_frame_from_swapchain(&targetFrame, &swapchainFrame);
        const auto inputWidth = mappedFrame.crop.x1 - mappedFrame.crop.x0;
        const auto inputHeight = mappedFrame.crop.y1 - mappedFrame.crop.y0;
        const auto inputAspect = inputHeight > 0.0F ? inputWidth / inputHeight : 1.0F;
        const auto outputAspect = static_cast<float>(outputWidth) / outputHeight;
        if (inputAspect > outputAspect) {
            const auto height = static_cast<float>(outputWidth) / inputAspect;
            targetFrame.crop.y0 = (outputHeight - height) * 0.5F;
            targetFrame.crop.y1 = targetFrame.crop.y0 + height;
        } else {
            const auto width = static_cast<float>(outputHeight) * inputAspect;
            targetFrame.crop.x0 = (outputWidth - width) * 0.5F;
            targetFrame.crop.x1 = targetFrame.crop.x0 + width;
        }

        updatePlaceboOverlay(std::chrono::steady_clock::now());
        pl_overlay_part overlayPart{};
        pl_overlay overlay{};
        if (statistics && overlayVisible && !overlayDisabled && placeboOverlayTexture) {
            overlayPart.src = {0.0F, 0.0F, static_cast<float>(overlayWidth),
                               static_cast<float>(overlayHeight)};
            overlayPart.dst = {10.0F, 10.0F, static_cast<float>(overlayWidth + 10),
                               static_cast<float>(overlayHeight + 10)};
            overlay.tex = placeboOverlayTexture;
            overlay.mode = PL_OVERLAY_NORMAL;
            overlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
            overlay.repr = pl_color_repr_rgb;
            overlay.color = pl_color_space_srgb;
            overlay.parts = &overlayPart;
            overlay.num_parts = 1;
            targetFrame.overlays = &overlay;
            targetFrame.num_overlays = 1;
        }

        const auto rendered =
            pl_render_image(placeboRenderer, &mappedFrame, &targetFrame, &pl_render_fast_params);
        const auto submitted = pl_swapchain_submit_frame(placeboSwapchain);
        pl_unmap_avframe(placeboVulkan->gpu, &mappedFrame);
        if (submitted) pl_swapchain_swap_buffers(placeboSwapchain);
        return rendered && submitted;
    }
#endif

    bool render(const PendingFrame& frame) {
#if defined(ECLIPSE_HAS_LIBPLACEBO)
        if (frame.avFrame) {
            const auto renderStarted = std::chrono::steady_clock::now();
            const bool rendered = renderPlacebo(frame.avFrame.get());
            if (!rendered && frame.hardwareDecoded) {
                hardwarePresentationFallbackRequested.store(true);
                return true;
            }
            if (rendered && statistics) {
                statistics->recordPresented(
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - renderStarted)
                            .count()),
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            renderStarted - frame.decodedAt)
                            .count()));
            }
            return rendered;
        }
#endif
        if (frame.hardwareFrame) {
#if defined(ECLIPSE_USE_VAAPI_X11)
            if (directPresentationEnabled.load() && renderVaapiSurface(frame)) return true;
#endif
            directPresentationEnabled.store(false);
            hardwarePresentationFallbackRequested.store(true);
            return true;
        }
        if (frame.hardwareDecoded) return false;
        if (!createSdlRenderer()) return false;
        const auto renderStarted = std::chrono::steady_clock::now();
        SDL_SetYUVConversionMode(frame.fullRange ? SDL_YUV_CONVERSION_JPEG
                                                  : SDL_YUV_CONVERSION_BT709);
        if (!texture || textureWidth != frame.width || textureHeight != frame.height) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
            if (!texture) return false;
            textureWidth = frame.width;
            textureHeight = frame.height;
        }
        if (SDL_UpdateYUVTexture(texture, nullptr, frame.y.data(), frame.width, frame.u.data(),
                                 frame.chromaWidth, frame.v.data(), frame.chromaWidth) < 0) {
            return false;
        }

        int outputWidth = 0;
        int outputHeight = 0;
        if (SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight) < 0) return false;
        if (outputWidth <= 0 || outputHeight <= 0) {
            processEvents();
            return true;
        }
        const double inputAspect = static_cast<double>(frame.width) / frame.height;
        const double outputAspect = static_cast<double>(outputWidth) / outputHeight;
        SDL_Rect destination{0, 0, outputWidth, outputHeight};
        if (inputAspect > outputAspect) {
            destination.h = static_cast<int>(outputWidth / inputAspect);
            destination.y = (outputHeight - destination.h) / 2;
        } else {
            destination.w = static_cast<int>(outputHeight * inputAspect);
            destination.x = (outputWidth - destination.w) / 2;
        }

        if (SDL_RenderClear(renderer) < 0 ||
            SDL_RenderCopy(renderer, texture, nullptr, &destination) < 0 ||
            !drawPerformanceOverlay()) {
            return false;
        }
        SDL_RenderPresent(renderer);
        if (statistics) {
            statistics->recordPresented(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - renderStarted)
                                               .count()),
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               renderStarted - frame.decodedAt)
                                               .count()));
        }
        return true;
    }

    int submit(PDECODE_UNIT decodeUnit) {
        if (!referenceFrameAccepted && decodeUnit->frameType != FRAME_TYPE_IDR) return DR_NEED_IDR;
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
            std::memcpy(packet->data + offset, entry->data,
                        static_cast<std::size_t>(entry->length));
            offset += entry->length;
        }
        packet->size = offset;
        packet->pts = decodeUnit->frameNumber;
        packet->dts = decodeUnit->frameNumber;
        if (decodeUnit->frameType == FRAME_TYPE_IDR) packet->flags |= AV_PKT_FLAG_KEY;

        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            av_packet_free(&packet);
            return DR_NEED_IDR;
        }
        int result = 0;
        auto decodeStarted = std::chrono::steady_clock::now();
        bool presentationFailed = false;
        bool decoderFallback = false;
        const auto receiveFrames = [&] {
            while (true) {
                result = avcodec_receive_frame(codecContext, frame);
                if (result != 0) break;
                if (statistics) {
                    statistics->recordDecoded(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - decodeStarted)
                        .count()));
                }
                const auto timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                           ? frame->best_effort_timestamp
                                           : frame->pts;
                auto color = std::find_if(frameColors.begin(), frameColors.end(),
                                          [timestamp](const auto& value) {
                                              return value.frameNumber == timestamp;
                                          });
                if (color == frameColors.end() && timestamp == AV_NOPTS_VALUE &&
                    !frameColors.empty()) {
                    color = frameColors.begin();
                }
                if (color != frameColors.end()) {
                    prepareFrameColor(frame, color->hdrActive, color->colorspace);
                    frameColors.erase(color);
                } else {
                    prepareFrameColor(frame, hdrModeRequested.load(), COLORSPACE_REC_709);
                }
                const auto queueResult = queueFrame(frame);
                if (queueResult == FrameQueueResult::decoderFallback) {
                    decoderFallback = true;
                    return;
                }
                if (queueResult == FrameQueueResult::failed) {
                    presentationFailed = true;
                    return;
                }
                consecutiveHardwareIdrFailures = 0;
                av_frame_unref(frame);
                decodeStarted = std::chrono::steady_clock::now();
            }
        };

        result = avcodec_send_packet(codecContext, packet);
        if (result == AVERROR(EAGAIN)) {
            receiveFrames();
            if (!presentationFailed && !decoderFallback && result == AVERROR(EAGAIN)) {
                result = avcodec_send_packet(codecContext, packet);
            }
        }
        if (result >= 0) {
            frameColors.push_back(
                {decodeUnit->frameNumber, decodeUnit->hdrActive, decodeUnit->colorspace});
            if (frameColors.size() > 64) frameColors.pop_front();
        }
        av_packet_free(&packet);
        if (decoderFallback) {
            av_frame_free(&frame);
            return DR_NEED_IDR;
        }
        if (presentationFailed) {
            av_frame_free(&frame);
            notify("connected", "SDL video presentation failed.");
            return DR_NEED_IDR;
        }
        if (result < 0) {
            referenceFrameAccepted = false;
            if (hardwareDecoderConfigured && decodeUnit->frameType == FRAME_TYPE_IDR &&
                ++consecutiveHardwareIdrFailures >= 2 &&
                fallBackToSoftwareDecoder("VAAPI repeatedly rejected IDR frames: " +
                                          ffmpegError(result) + ".")) {
                av_frame_free(&frame);
                return DR_NEED_IDR;
            }
            if (!hardwareDecoderConfigured) recoveryRequested.store(true);
            av_frame_free(&frame);
            notify("connected", "FFmpeg rejected encoded frame: " + ffmpegError(result));
            return DR_NEED_IDR;
        }
        if (decodeUnit->frameType == FRAME_TYPE_IDR) referenceFrameAccepted = true;

        receiveFrames();
        av_frame_free(&frame);
        if (decoderFallback) return DR_NEED_IDR;
        if (presentationFailed) {
            notify("connected", "SDL video presentation failed.");
            return DR_NEED_IDR;
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            referenceFrameAccepted = false;
            if (hardwareDecoderConfigured && decodeUnit->frameType == FRAME_TYPE_IDR &&
                ++consecutiveHardwareIdrFailures >= 2 &&
                fallBackToSoftwareDecoder("VAAPI repeatedly failed to decode IDR frames: " +
                                          ffmpegError(result) + ".")) {
                return DR_NEED_IDR;
            }
            if (!hardwareDecoderConfigured) recoveryRequested.store(true);
            notify("connected", "FFmpeg decode failed: " + ffmpegError(result));
            return DR_NEED_IDR;
        }
        return DR_OK;
    }

    StreamSettings settings;
    StatusListener listener;
    CloseListener closeListener;
    std::shared_ptr<StreamStatistics> statistics;
    OverlayListener overlayListener;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Uint32 rendererFlags = 0;
    SDL_Texture* texture = nullptr;
    SDL_Texture* overlayTexture = nullptr;
    InputForwarder inputForwarder;
    AVCodecContext* codecContext = nullptr;
    const AVCodec* decoderCodec = nullptr;
    AVBufferRef* hardwareDevice = nullptr;
    AVPixelFormat hardwarePixelFormat = AV_PIX_FMT_NONE;
    bool hardwareDecoderConfigured = false;
    int consecutiveHardwareIdrFailures = 0;
    std::atomic_bool hardwareDecodeActive{false};
    std::atomic_bool vaapiX11Device{false};
    std::atomic_bool directPresentationEnabled{false};
    std::atomic_bool directPresentationUsed{false};
    std::atomic_bool hardwarePresentationFallbackRequested{false};
    bool placeboActive = false;
#if defined(ECLIPSE_HAS_LIBPLACEBO)
    pl_log placeboLog = nullptr;
    pl_vk_inst placeboInstance = nullptr;
    VkSurfaceKHR placeboSurface = VK_NULL_HANDLE;
    pl_vulkan placeboVulkan = nullptr;
    pl_swapchain placeboSwapchain = nullptr;
    pl_renderer placeboRenderer = nullptr;
    std::array<pl_tex, 4> placeboTextures{};
    pl_tex placeboOverlayTexture = nullptr;
#endif
#if defined(ECLIPSE_USE_VAAPI_X11)
    Display* vaapiX11Display = nullptr;
    ::Window vaapiX11Window = 0;
    VADisplay vaapiDisplay = nullptr;
#endif
    int decoderWidth = 0;
    int decoderHeight = 0;
    std::thread renderThread;
    std::mutex initializationMutex;
    std::condition_variable initializationCondition;
    bool initializationComplete = false;
    std::string initializationError;
    std::mutex frameMutex;
    std::condition_variable frameCondition;
    std::condition_variable inputCondition;
    std::deque<PendingFrame> pendingFrames;
    std::array<std::optional<GamepadRumbleCommand>, 16> pendingRumble;
    std::array<std::optional<GamepadRumbleCommand>, 16> pendingTriggerRumble;
    std::array<std::optional<GamepadMotionCommand>, 32> pendingMotion;
    std::array<std::optional<GamepadLedCommand>, 16> pendingLed;
    bool stopping = false;
    bool renderThreadExited = false;
    bool inputEnabled = false;
    std::uint64_t inputRequestGeneration = 0;
    std::uint64_t inputAppliedGeneration = 0;
    bool overlayResumeRequested = false;
    bool streamOverlayActive = false;
    int textureWidth = 0;
    int textureHeight = 0;
    int overlayWidth = 0;
    int overlayHeight = 0;
    std::chrono::steady_clock::time_point nextOverlayUpdate{};
    int activeVideoFormat = VIDEO_FORMAT_H264;
    AVCodecID activeCodecId = AV_CODEC_ID_NONE;
    bool referenceFrameAccepted = false;
    bool sdlVideoInitialized = false;
    bool screenSaverDisabled = false;
    bool windowHidden = false;
    bool presentationErrorReported = false;
    bool presentationReported = false;
    bool overlayDisabled = false;
    bool overlayVisible = false;
    std::atomic_bool recoveryRequested{false};
    std::atomic_int recoveryDisplayIndex{-1};
    int currentDisplayIndex = 0;
    std::atomic_bool hdrModeRequested{false};
    std::deque<FrameColorInfo> frameColors;
    std::optional<std::string> previousMouseFocusClickthrough;
    SDL_YUV_CONVERSION_MODE previousYuvConversionMode = SDL_YUV_CONVERSION_AUTOMATIC;
    bool yuvConversionModeSaved = false;
};

VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener listener,
                               CloseListener closeListener,
                               std::shared_ptr<StreamStatistics> statistics,
                               OverlayListener overlayListener)
    : impl_(std::make_unique<Impl>(std::move(settings), std::move(listener),
                                     std::move(closeListener), std::move(statistics),
                                     std::move(overlayListener))) {}

VideoRenderer::~VideoRenderer() = default;

void VideoRenderer::initialize(int videoFormat, int width, int height, int frameRate) {
    if (!supportedVideoFormat(videoFormat)) {
        throw std::runtime_error("Linux renderer received an unsupported video profile.");
    }
    if (impl_->settings.enableHdr && (videoFormat & VIDEO_FORMAT_MASK_10BIT) == 0) {
        throw std::runtime_error("Host negotiated SDR video while Linux HDR was requested.");
    }
    if (impl_->settings.enableYuv444 && (videoFormat & VIDEO_FORMAT_MASK_YUV444) == 0) {
        throw std::runtime_error("Host negotiated YUV 4:2:0 while 4:4:4 was requested.");
    }
    impl_->initialize(videoFormat, width, height, frameRate);
}

int VideoRenderer::submit(PDECODE_UNIT decodeUnit) {
    return impl_->submit(decodeUnit);
}

bool VideoRenderer::recoveryRequired() const { return impl_->recoveryRequested.load(); }

std::optional<int> VideoRenderer::recoveryDisplayIndex() const {
    const auto index = impl_->recoveryDisplayIndex.load();
    return index >= 0 ? std::optional<int>{index} : std::nullopt;
}

void VideoRenderer::setInputEnabled(bool enabled) {
    impl_->setInputEnabled(enabled);
}

void VideoRenderer::setHdrMode(bool enabled) { impl_->setHdrMode(enabled); }

void VideoRenderer::resumeOverlay() { impl_->resumeOverlay(); }

void VideoRenderer::setGamepadRumble(std::uint16_t controllerNumber,
                                     std::uint16_t lowFrequency,
                                     std::uint16_t highFrequency) {
    impl_->setGamepadRumble(controllerNumber, lowFrequency, highFrequency);
}

void VideoRenderer::setGamepadTriggerRumble(std::uint16_t controllerNumber,
                                            std::uint16_t leftTrigger,
                                            std::uint16_t rightTrigger) {
    impl_->setGamepadTriggerRumble(controllerNumber, leftTrigger, rightTrigger);
}

void VideoRenderer::setGamepadMotionEventState(std::uint16_t controllerNumber,
                                                std::uint8_t motionType,
                                                std::uint16_t reportRateHz) {
    impl_->setGamepadMotionEventState(controllerNumber, motionType, reportRateHz);
}

void VideoRenderer::setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red,
                                  std::uint8_t green, std::uint8_t blue) {
    impl_->setGamepadLed(controllerNumber, red, green, blue);
}

}  // namespace eclipse

#else

namespace eclipse {
int selectVideoFormat(VideoCodec preference, int, bool enableHdr, bool enableYuv444) {
    if (preference == VideoCodec::hevc || preference == VideoCodec::av1 || enableHdr ||
        enableYuv444) {
        throw std::runtime_error("Requested video format is unavailable on this platform.");
    }
    return VIDEO_FORMAT_H264;
}
struct VideoRenderer::Impl {
    Impl(StreamSettings, StatusListener, CloseListener, std::shared_ptr<StreamStatistics>,
         OverlayListener) {}
};

VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener listener,
                               CloseListener closeListener,
                               std::shared_ptr<StreamStatistics> statistics,
                               OverlayListener overlayListener)
    : impl_(std::make_unique<Impl>(std::move(settings), std::move(listener),
                                     std::move(closeListener), std::move(statistics),
                                     std::move(overlayListener))) {}
VideoRenderer::~VideoRenderer() = default;
void VideoRenderer::initialize(int, int, int, int) {
    throw std::runtime_error("Native video output is unavailable on this platform.");
}
int VideoRenderer::submit(PDECODE_UNIT) {
    return DR_NEED_IDR;
}
bool VideoRenderer::recoveryRequired() const { return false; }

std::optional<int> VideoRenderer::recoveryDisplayIndex() const { return std::nullopt; }
void VideoRenderer::setInputEnabled(bool) {}
void VideoRenderer::setHdrMode(bool) {}
void VideoRenderer::resumeOverlay() {}
void VideoRenderer::setGamepadRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}
void VideoRenderer::setGamepadTriggerRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}
void VideoRenderer::setGamepadMotionEventState(std::uint16_t, std::uint8_t, std::uint16_t) {}
void VideoRenderer::setGamepadLed(std::uint16_t, std::uint8_t, std::uint8_t, std::uint8_t) {}
}  // namespace eclipse

#endif
