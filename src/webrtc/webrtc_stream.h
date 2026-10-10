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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webrtc_stream {
  /**
   * @brief A paired TV as shown in the Web UI.
   */
  struct paired_tv_t {
    std::string id;  ///< Client ID.
    std::string name;  ///< Display name.
    bool enabled = true;  ///< Whether the TV may connect.
  };

  /**
   * @brief A TV showing a PIN and waiting for it to be entered in the Web UI.
   */
  struct pending_tv_pairing_t {
    std::string id;  ///< Pairing request ID, 32 hex digits like a Moonlight request's.
    std::string name;  ///< Name sent by the TV.
    std::string address;  ///< TV address.
  };

  /**
   * @brief Run the TV signaling server until Sunshine shuts down.
   */
  void start();

  /**
   * @brief UDP ports WebRTC media may use, from `webrtc_media_port_min` and `webrtc_media_port_max`.
   *
   * A firewall then only needs that range open. 0 leaves its end of the range open: 1024 for the
   * first port and 65535 for the last.
   *
   * @param min First port, 0 for any.
   * @param max Last port, 0 for any.
   * @return First and last port, or nothing when both are 0 or the range is empty.
   */
  std::optional<std::pair<std::uint16_t, std::uint16_t>> media_port_range(int min, int max);

  /**
   * @brief Count TV streams that are capturing.
   * @return Number of active TV streams.
   */
  int session_count();

  /**
   * @brief List the TVs waiting to pair, for the Web UI's PIN page.
   * @return Pending TV pairing requests.
   */
  std::vector<pending_tv_pairing_t> pending_tv_pairings();

  /**
   * @brief Pair the TV that made a request if the PIN matches the one it shows.
   * @param pairing_id Pairing request ID.
   * @param pin PIN entered in the Web UI.
   * @param name Name to store; the TV's own name when empty.
   * @return Whether the TV paired, or nothing when no TV made that request.
   */
  std::optional<bool> approve_tv_pairing(std::string_view pairing_id, std::string_view pin, std::string_view name);

  /**
   * @brief Decline a TV's pairing request.
   * @param pairing_id Pairing request ID.
   * @return True when it was declined, or nothing when no TV made that request.
   */
  std::optional<bool> cancel_tv_pairing(std::string_view pairing_id);

  /**
   * @brief List paired TVs.
   * @return Paired TVs.
   */
  std::vector<paired_tv_t> paired_tvs();

  /**
   * @brief Forget one paired TV, disconnecting it if it is connected.
   * @param id Client ID.
   * @return True when the TV was paired, or nothing when the server is not running.
   */
  std::optional<bool> unpair_tv(std::string_view id);

  /**
   * @brief Allow or refuse a paired TV, disconnecting it when it is refused.
   * @param id Client ID.
   * @param enabled Whether the TV may connect.
   * @return True when the TV is paired, or nothing when the server is not running.
   */
  std::optional<bool> set_tv_enabled(std::string_view id, bool enabled);

  /**
   * @brief Forget every paired TV and disconnect any connected TV.
   * @return Number of TVs removed, or nothing when the server is not running.
   */
  std::optional<std::size_t> unpair_all_tvs();
}  // namespace webrtc_stream
