/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "crypto.h"
#include "input.h"
#include "terra_api.h"
#include "thread_safe.h"

namespace rtsp_stream {
  /**
   * @brief Display restoration trigger selected by an applied display profile.
   */
  enum class display_restore_e {
    host_default,  ///< Follow host display-device configuration.
    always,  ///< Restore after every transport end.
    on_stop,  ///< Restore only after application runtime termination.
    never,  ///< Keep applied display state after transport and runtime end.
  };

  constexpr auto RTSP_SETUP_PORT = 21;  ///< GameStream base-port offset used for the RTSP setup listener.

  /**
   * @brief RTSP launch session state shared with stream setup.
   */
  struct launch_session_t {
    uint32_t id;  ///< RTSP launch-session identifier assigned before stream startup.
    std::string session_id;  ///< Stable logical Terra session UUID.
    std::string stream_id;  ///< Unique Terra child-stream UUID.
    std::string display_id;  ///< Requested display resource UUID, empty when no resource is bound.
    std::string client_uuid;  ///< Persistent UUID of the paired client owning this session.
    std::string app_uuid;  ///< Stable UUID of the launched application.
    terra_api::input_permissions_t input_permissions;  ///< Input classes granted to this paired client.

    crypto::aes_t gcm_key;  ///< AES-GCM key negotiated for encrypted RTSP messages.
    crypto::aes_t iv;  ///< Initial RTSP AES-GCM IV supplied by the client.

    std::string av_ping_payload;  ///< AV ping payload.
    uint32_t control_connect_data;  ///< Client-provided token used when connecting the control channel.

    bool host_audio;  ///< Whether host audio should be played locally.
    bool terra {};  ///< Whether this transport was launched through Terra API v1.
    bool primary_stream {true};  ///< Whether this child owns audio and controller transport roles.
    std::string unique_id;  ///< Moonlight client unique identifier for this launch request.
    int width;  ///< Frame or display width in pixels.
    int height;  ///< Frame or display height in pixels.
    int fps;  ///< Requested video frame rate.
    int gcmap;  ///< Game controller mapping requested by the client.
    int appid;  ///< Application ID requested for launch or resume.
    int surround_info;  ///< Encoded GameStream surround-sound capability flags.
    std::string surround_params;  ///< Client-provided surround-sound layout parameters.
    bool continuous_audio;  ///< Whether audio packets continue during silence.
    bool enable_hdr;  ///< Whether HDR streaming is requested.
    bool enable_sops;  ///< Whether sequence output protection is requested.
    std::string client_name;  ///< Friendly client name from initial pairing.
    std::optional<int> profile_bitrate_kbps;  ///< Stream-profile bitrate override.
    std::optional<int> profile_video_format;  ///< Stream-profile codec override, using GameStream format IDs.
    std::optional<int> profile_chroma_sampling;  ///< Stream-profile chroma override.
    std::optional<int> profile_audio_channels;  ///< Stream-profile channel-count override.
    std::optional<int> profile_audio_mask;  ///< Stream-profile channel-mask override.
    input::mouse_mode_e profile_mouse_mode {input::mouse_mode_e::any};  ///< Stream-profile mouse-coordinate mode.
    bool profile_stream_applied {};  ///< Whether dimensions, frame rate, and HDR came from a stream profile.
    bool profile_encryption_required {};  ///< Whether negotiated audio and video encryption are mandatory.
    std::string capture_output_name;  ///< Exact display-device identifier selected by profile or workspace attachment.
    std::vector<std::string> app_arguments;  ///< Validated launch-profile arguments appended to application command.
    std::map<std::string, std::string, std::less<>> app_environment;  ///< Launch-profile environment overrides.
    std::optional<std::string> app_working_directory;  ///< Launch-profile working-directory override.
    std::optional<bool> app_elevated;  ///< Launch-profile elevation override.
    display_restore_e display_restore {display_restore_e::host_default};  ///< Applied display-profile restoration trigger.
    std::uint64_t telemetry_generation {1};  ///< Generation incremented whenever transport counters reset.

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;  ///< AES-GCM cipher used once encrypted RTSP is negotiated.
    std::string rtsp_url_scheme;  ///< URL scheme selected by the RTSP SETUP flow.
    uint32_t rtsp_iv_counter;  ///< Counter value mixed into encrypted RTSP IVs.
    std::string client_cert;  ///< PEM certificate for the paired Moonlight client.
    std::function<void()> timeout_cleanup;  ///< Cleanup invoked if RTSP never consumes this launch.
  };

  /**
   * @brief Immutable Terra view of one active streaming session.
   */
  struct session_info_t {
    std::string id;  ///< Stable logical session UUID.
    std::string stream_id;  ///< Unique child transport UUID.
    std::string display_id;  ///< Bound display resource UUID, empty when no resource is bound.
    bool terra;  ///< Whether this transport belongs to a Terra API session.
    bool primary;  ///< Whether this child owns primary stream roles.
    std::string client_uuid;  ///< Persistent owner client UUID.
    std::string app_uuid;  ///< Stable application UUID.
    int legacy_app_id;  ///< Legacy numeric GameStream application ID.
    std::string state;  ///< Explicit stream lifecycle state.
    std::chrono::system_clock::time_point started_at;  ///< Time at which stream state was allocated.
    int width;  ///< Negotiated capture width.
    int height;  ///< Negotiated capture height.
    int fps;  ///< Negotiated refresh rate.
    bool hdr;  ///< Whether HDR was requested.
    std::uint64_t telemetry_generation;  ///< Current counter generation.
    std::string codec;  ///< Active encoded video codec.
    int bitrate_kbps;  ///< Average transmitted video bitrate.
    std::uint64_t captured_frames;  ///< Fresh frames received from capture provider.
    std::uint64_t encoded_frames;  ///< Frames successfully produced by encoder.
    std::uint64_t dropped_frames;  ///< Captured frames dropped before encoding.
    std::uint64_t transmitted_frames;  ///< Frames transmitted to client.
    std::uint64_t video_bytes;  ///< Encoded video payload bytes transmitted.
    std::uint64_t audio_bytes;  ///< Encoded audio payload bytes transmitted.
    std::uint64_t control_bytes;  ///< Control transport bytes received.
    std::uint64_t input_bytes;  ///< Authenticated input plaintext bytes received.
    std::uint64_t queue_depth;  ///< Latest captured-frame queue depth.
    std::uint64_t queue_drops;  ///< Captured frames discarded by queue overflow.
    bool interval_sampled;  ///< Whether interval rates have a complete observation window.
    bool capture_active;  ///< Whether capture provider reported during latest interval.
    bool audio_active;  ///< Whether encoded audio advanced during latest interval.
    bool capture_latency_sampled;  ///< Whether capture provider supplied a frame timestamp.
    bool encode_latency_sampled;  ///< Whether one encoder invocation latency was observed.
    double capture_fps;  ///< Fresh capture frame rate over latest interval.
    double encode_fps;  ///< Encoded frame rate over latest interval.
    double transmit_fps;  ///< Transmitted frame rate over latest interval.
    double capture_latency_ms;  ///< Latest capture-to-encode frame age.
    double encode_latency_ms;  ///< Latest encoder-only processing latency.
  };

  /**
   * @brief Queue a launch session until the RTSP client connects.
   *
   * @param launch_session Session state prepared by the GameStream launch handler.
   * @return `true` when queued, or `false` when another launch is pending.
   */
  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();
  /**
   * @brief Return immutable snapshots of active streaming sessions.
   *
   * @return Active session snapshots.
   */
  std::vector<session_info_t> sessions();
  /**
   * @brief Return one immutable snapshot per active transport.
   *
   * @return Active child-stream snapshots without logical-session coalescing.
   */
  std::vector<session_info_t> transport_sessions();
  /** @brief Advance fixed-window telemetry rates for every active stream. */
  void sample_telemetry();
  /**
   * @brief Terminate active streams for one logical Terra session.
   *
   * @param session_id Stable logical session UUID.
   * @param client_uuid Required owner UUID; empty permits administrative control.
   * @return `true` when at least one matching stream was terminated.
   */
  bool terminate_session(std::string_view session_id, std::string_view client_uuid);

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();
  /**
   * @brief Terminate active sessions associated with a client certificate.
   *
   * @param cert Certificate data or object used by the operation.
   */
  void terminate_sessions_by_cert(std::string_view cert);

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
