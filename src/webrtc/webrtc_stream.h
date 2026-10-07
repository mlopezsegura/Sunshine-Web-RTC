/**
 * @file src/webrtc/webrtc_stream.h
 * @brief Declarations for streaming to Moonlight WebRTC TVs directly from Sunshine.
 *
 * Sunshine serves the Moonlight WebRTC Tizen client itself: the TV signals over a WebSocket and
 * receives Sunshine's encoded video and Opus audio over WebRTC, without a GameStream hop through a
 * separate Gateway process. Encoded frames go from the encoder straight to the RTP packetizer.
 */
#pragma once

// standard includes
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace webrtc_stream {
  /**
   * @brief A paired TV as shown in the Web UI.
   */
  struct paired_tv_t {
    std::string id;  ///< Client ID.
    std::string name;  ///< Display name.
  };

  /**
   * @brief Outcome of opening a TV pairing window.
   */
  struct pairing_t {
    bool ok;  ///< Whether a window was opened.
    std::string pin;  ///< PIN to show; empty on failure.
    std::string message;  ///< User-safe status.
  };

  /**
   * @brief Run the TV signaling server until Sunshine shuts down.
   */
  void start();

  /**
   * @brief Count TV streams that are capturing.
   * @return Number of active TV streams.
   */
  int session_count();

  /**
   * @brief Open a two-minute window in which one TV may pair.
   * @return The PIN to show, which is never logged.
   */
  pairing_t open_tv_pairing();

  /**
   * @brief Report the state of the TV pairing window.
   * @return One of "tv-pairing-idle", "tv-pairing-waiting", "tv-paired", "tv-pairing-expired" or "tv-pairing-locked".
   */
  std::string tv_pairing_status();

  /**
   * @brief List paired TVs.
   * @return Paired TVs.
   */
  std::vector<paired_tv_t> paired_tvs();

  /**
   * @brief Forget every paired TV and disconnect any connected TV.
   * @return Number of TVs removed, or nothing when the server is not running.
   */
  std::optional<std::size_t> unpair_all_tvs();
}  // namespace webrtc_stream
