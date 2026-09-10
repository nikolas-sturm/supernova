/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;  ///< GameStream base-port offset used for the video UDP stream.
  constexpr auto CONTROL_PORT = 10;  ///< GameStream base-port offset used for the control channel.
  constexpr auto AUDIO_STREAM_PORT = 11;  ///< GameStream base-port offset used for the audio UDP stream.

  struct session_t;

  /** @brief Capture counters shared with asynchronous provider callbacks. */
  struct capture_telemetry_t {
    std::atomic_uint64_t events {};  ///< Capture-provider callback count.
    std::atomic_uint64_t captured_frames {};  ///< Fresh captured frame count.
    std::atomic_uint64_t dropped_frames {};  ///< Captured frames dropped before encoding.
    std::atomic_uint64_t queue_depth {};  ///< Latest capture queue depth.
    std::atomic_uint64_t queue_drops {};  ///< Capture queue overflow count.
  };

  /**
   * @brief Stream configuration shared by capture and network senders.
   */
  struct config_t {
    audio::config_t audio;  ///< Audio capture configuration for the stream.
    video::config_t monitor;  ///< Video capture and encoder configuration for the selected monitor.

    int packetsize;  ///< Maximum payload size for network packets.
    int minRequiredFecPackets;  ///< Minimum recovery packets required before FEC is emitted.
    int mlFeatureFlags;  ///< Moonlight feature flags negotiated for this session.
    int controlProtocolType;  ///< GameStream control protocol variant selected by the client.
    int audioQosType;  ///< Audio QoS type.
    int videoQosType;  ///< Video QoS type.

    uint32_t encryptionFlagsEnabled;  ///< Bitmask of GameStream encryption features enabled for the session.

    std::optional<int> gcmap;  ///< Optional game-controller mapping override from the launch request.
  };

  namespace session {
    /**
     * @brief Enumerates supported state options.
     */
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Allocate and initialize platform input state for a stream.
     *
     * @param config Configuration values to apply.
     * @param launch_session Launch session.
     * @return Allocated object or identifier, or an error value on failure.
     */
    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    /**
     * @brief Start a streaming session for the supplied peer address.
     *
     * @param session Active streaming or pairing session for the request.
     * @param addr_string Addr string.
     * @return Start status.
     */
    int start(session_t &session, const std::string &addr_string);
    /**
     * @brief Stop a streaming session and prevent more packets from being queued.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void stop(session_t &session);
    /**
     * @brief Wait for worker threads owned by the session to exit.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void join(session_t &session);
    /**
     * @brief Platform handle returned from stream setup.
     *
     * @param session Active streaming or pairing session for the request.
     * @return Current lifecycle state for the stream session.
     */
    state_e state(session_t &session);
    /**
     * @brief Return the paired client certificate for a stream session.
     *
     * @param session Active streaming or pairing session for the request.
     * @return PEM certificate associated with the session's client.
     */
    const std::string &client_cert(session_t &session);
    /** @brief Return lifetime-safe capture telemetry. @param session Active stream session. @return Shared counters. */
    std::shared_ptr<capture_telemetry_t> capture_telemetry(session_t &session);
    /** @brief Record one fresh captured frame. @param session Active stream session. */
    void record_captured_frame(session_t &session);
    /** @brief Record one successfully encoded frame. @param session Active stream session. */
    void record_encoded_frame(session_t &session);
    /** @brief Record captured frames dropped before encoding. @param session Active stream session. @param count Number of frames. */
    void record_dropped_frame(session_t &session, std::uint64_t count = 1);
    /** @brief Record captured-frame queue state. @param session Active stream session. @param depth Current depth. @param dropped Frames discarded by this update. */
    void record_capture_queue(session_t &session, std::uint64_t depth, std::uint64_t dropped);
    /** @brief Record latest capture-to-encode frame age. @param session Active stream session. @param latency Capture frame age, or empty when provider supplied no timestamp. */
    void record_capture_latency(session_t &session, std::optional<std::chrono::steady_clock::duration> latency);
    /** @brief Record latest encoder-only latency. @param session Active stream session. @param latency Encoder duration. */
    void record_encode_latency(session_t &session, std::chrono::steady_clock::duration latency);
    /** @brief Advance cached interval telemetry rates. @param session Active stream session. */
    void sample_telemetry(session_t &session);
    /**
     * @brief Build an immutable Terra snapshot for a stream session.
     *
     * @param session Active stream session.
     * @return Session metadata safe to use after releasing registry lock.
     */
    rtsp_stream::session_info_t snapshot(session_t &session);
    /** @brief Complete deferred display and input restoration after application runtime termination. */
    void runtime_stopped();
  }  // namespace session
}  // namespace stream
