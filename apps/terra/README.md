# Terra

GPLv3 game-streaming client under active construction. Neutralinojs hosts React UI; native C++
extension owns Sol control, Moonlight transport, decoding, rendering, audio, and input.

## Current slice

- Neutralinojs 6.9 shell with Vite 8, React 19, React Compiler, and TypeScript 7.
- CSS Modules UI with local host entry and typed Zustand state.
- Zod-validated Neutralino extension events.
- C++23 extension connected over Neutralino's authenticated localhost WebSocket.
- `moonlight-common-c` built and linked from revision pinned by Moonlight Qt.
- Native host repository persisted under the operating system's application data path, with optional
  bounded `_nvstream._tcp.local` DNS-SD discovery across active IPv4 interfaces.
- Bounded asynchronous Sol `serverinfo` probes with IPv4, IPv6, DNS name, and custom-port support.
- Moonlight-compatible PIN pairing with transcript verification and pinned mutual TLS.
- Persistent RSA client identity; Windows protects the private key at rest with DPAPI.
- Paired application listing with progressive, persistent box-art caching.
- Authenticated host MAC capture and rate-limited Wake-on-LAN for offline paired hosts.
- Native Sol launch, resume, and cancel control with retained session cryptographic context.
- `moonlight-common-c` RTSP/control/video/audio/input transport startup and deterministic teardown.
- Windows H.264, HEVC, and AV1 hardware decode through FFmpeg D3D11VA with direct D3D11
  presentation and HDR10 output for Main10 streams.
- Linux H.264, HEVC, and AV1 VAAPI decode with automatic software fallback, SDL2 SDR output, and
  optional Vulkan/libplacebo HDR10 or YUV 4:4:4 presentation.
- FFmpeg Opus decode and stereo, 5.1, or 7.1 float output with WASAPI on Windows and SDL2 on Linux.
- Native keyboard/mouse forwarding through Win32 raw input or SDL2, plus multi-gamepad XInput or
  SDL2 forwarding.
- Persisted Moonlight-style stream settings with custom modes and target-display selection.
- Optional blocked-port diagnostics for failed Moonlight connection stages and transport failures.
- Route-aware packet sizing that reduces Moonlight video packets on VPN and low-MTU interfaces.
- Native D3D11/SDL performance overlay with video rate, bitrate, frame loss, latency, decode,
  presentation, and queue-drop metrics.
- Biome, Vitest, and Testing Library quality gates.

Session launch starts or resumes the selected host application, retains its RTSP URL and remote-input
key in native memory, and calls `LiStartConnection`. Windows builds decode H.264, HEVC, or AV1 with
D3D11VA and present through a dedicated native Win32/D3D11 stream window. Linux builds prefer FFmpeg
VAAPI decode, fall back to software decoding, and present through SDL2. Opus packets decode to bounded
PCM and play through the default WASAPI or SDL2 output device. Keyboard, mouse, and connected gamepads
forward directly through `moonlight-common-c` without crossing Neutralino IPC.

## Prerequisites

- Node.js 20.19+, 22.12+, or newer
- CMake 3.24+
- C++23 compiler
- OpenSSL development package discoverable by CMake
- Platform webview required by Neutralinojs

Linux native media additionally requires `pkg-config`, SDL2 2.0.16+, FFmpeg `libavcodec` 59+,
FFmpeg `libavutil` 57+, FFmpeg `libswresample` 4+, GTK 3.22+, WebKitGTK 2.40+, Wayland client, and
Wayland protocols development packages.
Optional `libva`, `libva-x11`, and X11 development packages enable direct VAAPI surface presentation.
Optional libplacebo 7+, Vulkan, and FFmpeg `libavformat` 59+ development packages enable Linux HDR10
and YUV 4:4:4 output.

The Windows native renderer requires Windows 10 version 1607 or newer. Monorepo
builds use MSYS2 UCRT64 through `tooling/native/build.mjs`, not a Visual Studio
developer shell. See the root development guide for current workspace commands.

Headless Linux Sol hosts need a real Xorg server with an input driver. Xvfb can provide video
capture but does not consume Sol's hot-plugged `/dev/uinput` keyboard and mouse devices. Audio
also requires Sol's `stream_audio` setting to be enabled and a working PulseAudio/PipeWire sink.

## Setup

```sh
git submodule update --init --recursive
npm install
npm run dev
```

`npm run dev` rebuilds the debug native extension, starts Vite, and launches Neutralino. Frontend
changes hot-reload. Restart the command after changing native C++ or CMake; Neutralino does not
hot-restart an already running extension.

Frontend-only browser preview:

```sh
npm run dev:web
```

Browser preview reports native core unavailable by design.

## Quality checks

```sh
npm run check
npm run build:web
npm run core:build
ctest --test-dir native/build --output-on-failure
```

Create portable Neutralino bundles:

```sh
npm run bundle
```

Windows MinGW builds stage their compiler, OpenSSL, and transitive runtime DLLs beside the native
extension. Do not remove these files from the distribution; otherwise Windows may load incompatible
DLLs from another program on `PATH`.

## Upstream updates

Moonlight Qt is a root submodule. Its own pinned submodules include `moonlight-common-c`, patched
ENet, NanoRS, qmdnsengine, and SDL game controller mappings.

```sh
git -C refs/moonlight-qt fetch origin
git -C refs/moonlight-qt checkout origin/master
git submodule update --init --recursive
npm run core:build
```

Review and commit root submodule pointer only after native build and stream regression tests pass.
Do not independently advance nested `moonlight-common-c`; Moonlight Qt pins a compatible revision.

## Source boundaries

- `src/`: React frontend and low-rate bridge protocol.
- `native/`: C++ extension and CMake integration.
- `extensions/terra-core/bin/`: generated extension binary consumed by Neutralino.
- `refs/moonlight-qt/`: untouched upstream source and recursive dependencies.

Video frames, audio packets, and high-rate input must remain in native code. Neutralino IPC carries
commands, state, and telemetry only.

## Native host protocol

The frontend dispatches host, pairing, app-list, launch, and session-cancel commands. Native state is
broadcast through versioned, Zod-validated events. Host additions are saved before probing, so
unreachable machines remain configured and can be refreshed later.

Only unauthenticated HTTP `serverinfo` runs before pairing. Authenticated endpoints such as
`applist`, launch, and cancel must use mutual TLS with the exact certificate established by the
pairing challenge. Never replace that pin with disabled TLS verification.

Pairing follows Moonlight's challenge protocol: salted PIN key derivation, encrypted bidirectional
challenges, server RSA signature verification, client secret signing, and a final mutual-TLS
challenge. A server certificate is persisted only after every stage succeeds.

Box art is fetched through pinned mutual TLS, validated as PNG or JPEG, limited to 8 MiB, then cached
under the application data directory. Artwork is low-rate control-plane data; video and audio never
cross Neutralino IPC.

The Settings view persists a local stream profile and applies it on the next launch. Preset and
custom resolution, frame rate, bitrate, fullscreen/borderless/windowed presentation, target display,
V-Sync, stereo/5.1/7.1 audio, host audio mute, Sol game optimizations, connection warnings,
display-awake behavior, absolute mouse mode, mouse button and wheel transforms, controller
face-button layout, forced controller presence, background controller input, and codec selection are
wired to native behavior. Controls that need new transport or platform work remain visible but
disabled with a reason instead of silently accepting unsupported values.

Automatic discovery listens only while enabled. Advertised endpoints are treated as untrusted and
persisted only after a compatible Sol `serverinfo` response succeeds. Blocked-port diagnostics
  use Moonlight's external connectivity tester only when enabled. Performance statistics are sampled
  once per second in native core; same cached snapshot feeds native HUD and versioned quick-menu
  telemetry. React keeps only latest 60 samples.

Surround output is preflighted against the default audio endpoint before Sol launch. Unsupported
5.1 or 7.1 layouts downgrade to stereo instead of starting a silent stream.

Ending a stream stops Moonlight transport and leaves the Sol app available to resume by
default. Enable the host quit setting to end the remote application too.

Windows video output uses the pinned FFmpeg 8.1.2 shared development build from gyan.dev. CMake
verifies its SHA-256 before extraction and stages only the executable's resolved DLL closure. Closing
the stream window disconnects transport and leaves the host application available to resume.
Linux video output links distro-provided FFmpeg and SDL2 libraries. It attempts VAAPI decoding,
retains decoded VA surfaces, and presents them directly with `vaPutSurface` on native X11 when the
statistics overlay is disabled. Wayland and XWayland use Vulkan/libplacebo when available, including
direct DRM PRIME import of VAAPI frames. Hardware-decoded video surfaces are never downloaded to CPU
memory: unavailable or failed GPU presentation explicitly switches the decoder to software before
SDL presentation. Software decoding also remains the fallback when VAAPI or its selected codec
profile is unavailable. A three-frame render queue drops oldest frames on sustained overflow.
Automatic codec selection prefers HEVC, then AV1, on Windows when host and D3D11 GPU report matching
profile support. HDR selects a Main10 stream and Rec. 2020 PQ 10-bit swapchain; Windows HDR must be
enabled for the target display. V-Sync is bypassed when stream FPS exceeds active display refresh so
presentation cannot block decoding. Exclusive fullscreen selects the highest cadence-matched refresh
at desktop resolution, or the closest suitable resolution when necessary; DXGI and SDL restore the
desktop mode on exit. Borderless and windowed modes never switch display mode. Linux automatic mode
prefers lower-complexity H.264; HEVC and AV1 remain available explicitly. On Linux, Main10 HDR and
8/10-bit 4:4:4 frames use Vulkan/libplacebo so swapchain color space, PQ transfer, BT.2020 primaries,
and Sol HDR metadata reach compatible compositors and displays. Vulkan presentation composites
the performance overlay on GPU without transferring video frames through CPU memory.
Linux audio uses SDL2's default output device with a 50 ms PCM queue. Stereo, 5.1, and 7.1 preserve
Moonlight channel masks. Missing Opus packets become
duration-preserving silence; oldest samples are dropped if network jitter exceeds the queue bound.

WASAPI runs event-driven in shared mode at 48 kHz float with the selected channel layout. Its native
queue is capped at 50 ms; oldest samples are dropped on overflow instead of allowing latency to grow
without bound.

In game-oriented mouse mode, click the native stream window to capture input; press
`Ctrl+Alt+Shift+Z` or change focus to release it. Windows uses raw mouse reports; Linux uses SDL2
relative mode. Remote-desktop
mouse mode sends absolute pointer positions without capture. Focus loss releases every forwarded key
and mouse button to prevent stuck remote input. Gamepad input supports up to four XInput controllers
on Windows and sixteen SDL2 controllers on Linux, including stable slots, hot-plug, standard buttons,
analog sticks, and triggers. Linux builds bundle Moonlight's SDL controller mapping database.
Controller rumble routes by slot; SDL2 trigger rumble is enabled when supported. A controller enqueue
failure drops that update without disabling other controllers or future sessions.
On Linux, SDL2 controllers also expose Sol touchpads, host-requested accelerometer and gyroscope
reports, battery state, and RGB LED control when supported by hardware. XInput on Windows remains
limited to standard controls and whole-controller rumble.
Touchscreens can act as one-finger relative trackpads with one- and two-finger taps, or send direct
Sol touch input with mouse fallback for older hosts. Windows also forwards native pen pressure,
tilt, rotation, eraser, and barrel-button state. Direct coordinates exclude letterboxed video margins.
System-key capture can be disabled, limited to fullscreen, or enabled whenever input is captured.
Linux uses SDL keyboard grab and Windows uses a low-level keyboard hook; secure-desktop combinations
such as `Ctrl+Alt+Del` remain reserved by the operating system.

Moonlight's PC shortcuts are supported: `Ctrl+Alt+Shift+Q` disconnects, `Z` toggles input capture,
`X` toggles fullscreen, `S` toggles performance statistics, `M` toggles mouse mode, `V` sends up to
16 KiB of validated UTF-8 clipboard text, `D` minimizes, `C` toggles the local cursor in direct mouse
mode, and `L` locks the pointer to the video area in direct mouse mode. Each shortcut requires the
full `Ctrl+Alt+Shift` chord, remains local, and releases forwarded modifier state before acting.
`Ctrl+Alt+Shift+O` opens the stream overlay while the stream has focus and input is captured. On
Wayland, the stream overlay requires a compositor with wlr layer-shell support; Terra opens a native
Wayland surface on the compositor's overlay layer above the focused output, including when the SDL
renderer runs through XWayland. Outside fullscreen, the testing overlay covers the full focused output.

Wake-on-LAN MAC addresses are accepted only from pinned, mutually authenticated host probes and are
never exposed to the webview. Terra sends bounded UDP port 9 broadcasts and rate-limits requests.

Before transport startup, Terra inspects the operating system route to the host. Native LAN routes
use 1392-byte video packets, likely VPN routes use Moonlight's 1024-byte remote
policy, and unknown routes retain `moonlight-common-c` automatic remote detection. DNS names are
resolved only to detect VPN routes; they never force local policy because transport resolves them
independently.

On Windows, `%APPDATA%/Eclipse/identity-key.dpapi` is encrypted for the current Windows user. The
core writes a restricted runtime PEM only while running because the TLS adapter requires a key
file, then removes it during clean shutdown. Keep `identity-cert.pem`, `identity-key.dpapi`, and
`client-state.json` together when backing up identity. Losing identity requires pairing again.

## Rename compatibility

Terra's executable, extension ID (`dev.terra.core`), native namespace, CMake targets,
`TERRA_*` variables, and app-private IPC/overlay messages use the new name. Frontend,
core, and Linux overlay must be deployed together; old staged binaries are not compatible.
Overlay storage keys are transient session coordination, not user preferences, and are
renamed without migrating stale requests. No native build or staging tree is renamed.

The following names deliberately remain stable to preserve existing user data:

- Neutralino `applicationId`: `dev.eclipse.client`, retaining system storage and window state.
- Stream profile storage key: `eclipse-client-settings` in Neutralino and browser storage.
- Native identity/host data: `${NL_OSDATAPATH}/Eclipse` and standalone `.eclipse-data`.
  Keeping these paths preserves pairing identities, host certificate pins, and cached artwork.
  No identities or private keys are copied or regenerated by the rename.
- Persisted theme selectors, including `moonlight`: all 21 palettes and the `dark`
  default remain. Shared branding uses `AppBrand` with explicit product names and `data-terra-button`;
  the old component and button attribute have no compatibility aliases.

Upstream Moonlight dependency names, revision metadata, protocol/API references, URLs,
licenses, and `refs/moonlight-qt` are unchanged. Imported unsupported-platform configuration
(`commandDarwin`) is retained verbatim, not supported or added to CI. The old generated
`extensions/eclipse-core/bin` directory remains ignored and untouched.

The root lockfile, native staging fixtures, shared branding, and workspace documentation
now use `terra-client`, `terra-core.exe`, `TERRA_STAGE_*`, and
`apps/terra/extensions/terra-core/bin` consistently. Rebuild the native extension before
launching or packaging; existing generated binaries still carry their previous names.
Existing local CMake overrides must use `TERRA_*` instead of `ECLIPSE_*` when reconfigured;
dependency-specific `MOONLIGHT_QT_ROOT`/`MOONLIGHT_COMMON_ROOT` names remain unchanged.

## License

Terra is licensed under GNU GPL version 3. Moonlight Qt is GPLv3; moonlight-common-c carries its
own GPLv3 terms. Full upstream license text is available at `refs/moonlight-qt/LICENSE` and will be
included in release bundles before distribution.
