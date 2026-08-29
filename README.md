# Eclipse

GPLv3 game-streaming client under active construction. Neutralinojs hosts React UI; native C++
extension owns Sunshine control, Moonlight transport, decoding, rendering, audio, and input.

## Current slice

- Neutralinojs 6.9 shell with Vite 8, React 19, React Compiler, and TypeScript 7.
- CSS Modules UI with local host entry and typed Zustand state.
- Zod-validated Neutralino extension events.
- C++20 extension connected over Neutralino's authenticated localhost WebSocket.
- `moonlight-common-c` built and linked from revision pinned by Moonlight Qt.
- Native manual-host repository persisted under the operating system's application data path.
- Bounded asynchronous Sunshine `serverinfo` probes with IPv4, IPv6, DNS name, and custom-port support.
- Moonlight-compatible PIN pairing with transcript verification and pinned mutual TLS.
- Persistent RSA client identity; Windows protects the private key at rest with DPAPI.
- Paired application listing with progressive, persistent box-art caching.
- Native Sunshine launch, resume, and cancel control with retained session cryptographic context.
- `moonlight-common-c` RTSP/control/video/audio/input transport startup and deterministic teardown.
- Windows H.264 hardware decode through FFmpeg D3D11VA with Rec. 709 limited-range conversion and direct D3D11 presentation.
- FFmpeg Opus decode, stereo float conversion, and event-driven WASAPI shared-mode playback.
- Native Win32 keyboard/raw-mouse forwarding and single-controller XInput forwarding.
- Persisted Moonlight-style stream settings with explicit live and planned capability states.
- Biome, Vitest, and Testing Library quality gates.

Session launch starts or resumes the selected host application, retains its RTSP URL and remote-input
key in native memory, and calls `LiStartConnection`. Windows builds decode H.264 with D3D11VA and
present through a dedicated native Win32/D3D11 stream window. Opus packets decode to bounded stereo
PCM and play through the default Windows output device. Keyboard, mouse, and the first connected
XInput controller forward directly through `moonlight-common-c` without crossing Neutralino IPC.

## Prerequisites

- Node.js 20.19+, 22.12+, or newer
- CMake 3.24+
- C++20 compiler
- OpenSSL development package discoverable by CMake
- Platform webview required by Neutralinojs

On Windows, run commands from a Visual Studio developer shell when CMake cannot locate MSVC.

Headless Linux Sunshine hosts need a real Xorg server with an input driver. Xvfb can provide video
capture but does not consume Sunshine's hot-plugged `/dev/uinput` keyboard and mouse devices. Audio
also requires Sunshine's `stream_audio` setting to be enabled and a working PulseAudio/PipeWire sink.

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
- `extensions/eclipse-core/bin/`: generated extension binary consumed by Neutralino.
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

The Settings view persists a local stream profile and applies it on the next launch. Resolution,
frame rate, bitrate, fullscreen/borderless/windowed presentation, V-Sync, host audio mute, Sunshine
game optimizations, connection warnings, display-awake behavior, absolute mouse mode, mouse button
and wheel transforms, controller face-button layout, forced controller presence, background
controller input, and H.264 hardware-decoder selection are wired to native behavior. Controls that
need new transport or platform work remain visible but disabled with a reason instead of silently
accepting unsupported values.

Cancel stops Moonlight transport before ending the Sunshine app and is available for sessions
launched or resumed by the current Eclipse process.

Windows video output uses the pinned FFmpeg 8.1.2 shared development build from gyan.dev. CMake
verifies its SHA-256 before extraction and stages only the executable's resolved DLL closure. Closing
the stream window hides it; stop the active session from Eclipse to tear down transport and host app.

WASAPI runs event-driven in shared mode at 48 kHz float stereo. Its native queue is capped at 500 ms;
oldest samples are dropped on overflow instead of allowing latency to grow without bound.

In game-oriented mouse mode, click the native stream window to capture input; press `F8` or change
focus to release it. Relative and absolute Windows raw-mouse reports are normalized to relative host
movement. Remote-desktop mouse mode sends absolute pointer positions without capture. Focus loss
also releases every forwarded key and mouse button to prevent stuck remote input. Gamepad input
currently supports the first attached XInput controller without rumble.

On Windows, `%APPDATA%/Eclipse/identity-key.dpapi` is encrypted for the current Windows user. The
core writes a restricted runtime PEM only while running because the TLS adapter requires a key
file, then removes it during clean shutdown. Keep `identity-cert.pem`, `identity-key.dpapi`, and
`client-state.json` together when backing up identity. Losing identity requires pairing again.

## License

Eclipse is licensed under GNU GPL version 3. Moonlight Qt is GPLv3; moonlight-common-c carries its
own GPLv3 terms. Full upstream license text is available at `refs/moonlight-qt/LICENSE` and will be
included in release bundles before distribution.
