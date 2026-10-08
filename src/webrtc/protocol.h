/**
 * @file src/webrtc/protocol.h
 * @brief Declarations for the Moonlight WebRTC TV protocol (version 2).
 *
 * This is the JSON protocol spoken by the Moonlight WebRTC Tizen client over its signaling
 * WebSocket. Sunshine implements it directly, so the TV needs no separate Gateway process.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace webrtc_stream::protocol {
  constexpr int VERSION = 2;  ///< Protocol version; version 2 made TV authentication mandatory.

  /**
   * @brief Video codecs the TV can request.
   */
  enum class video_codec_e {
    h264,  ///< H.264 High 8-bit.
    hevc,  ///< HEVC Main, or Main10 with HDR.
    av1,  ///< AV1 Main 8-bit, or 10-bit with HDR.
  };

  /**
   * @brief One resolution the TV may choose, with the codecs it may combine with it.
   */
  struct video_mode_t {
    int width;  ///< Width in pixels.
    int height;  ///< Height in pixels.
    int fps;  ///< Frame rate.
    bool supports_h264;  ///< Whether H.264 is offered at this resolution.
    bool supports_hevc;  ///< Whether HEVC is offered at this resolution.
    bool supports_av1;  ///< Whether AV1 is offered at this resolution, when the encoder supports it.
    video_codec_e default_codec;  ///< Codec preselected by the TV.
    int default_bitrate_kbps;  ///< Bitrate preselected by the TV.
    bool experimental;  ///< Whether the TV labels the resolution as experimental.
    bool supports_hdr;  ///< Whether HDR may be selected at this resolution.
  };

  /**
   * @brief Stream settings requested by the TV and validated against the supported modes.
   */
  struct stream_settings_t {
    int width = 1280;  ///< Width in pixels.
    int height = 720;  ///< Height in pixels.
    int fps = 60;  ///< Frame rate.
    int bitrate_kbps = 12000;  ///< Total stream bitrate in kilobits per second.
    video_codec_e codec = video_codec_e::h264;  ///< Video codec.
    bool hdr = false;  ///< Whether a 10-bit BT.2020 PQ stream is requested.
    int audio_channels = 2;  ///< Audio channel count; only stereo is supported.

    /**
     * @brief Compare two settings field by field.
     * @return True when every field matches.
     */
    bool operator==(const stream_settings_t &) const = default;
  };

  /**
   * @brief Resolutions offered to the TV. 1440p is not in Samsung's Cloud Gaming table.
   */
  constexpr std::array SUPPORTED_VIDEO_MODES {
    video_mode_t {1280, 720, 60, true, true, true, video_codec_e::h264, 12000, false, false},
    video_mode_t {1920, 1080, 60, true, true, true, video_codec_e::h264, 20000, false, true},
    video_mode_t {2560, 1440, 60, true, true, true, video_codec_e::hevc, 30000, true, true},
    video_mode_t {3840, 2160, 60, false, true, true, video_codec_e::hevc, 50000, false, true},
  };

  /**
   * @brief Bitrates the TV may select. Above 50 Mbps a wired TV may hit its 100 Mbps port.
   */
  constexpr std::array SUPPORTED_BITRATES_KBPS {10000, 12000, 15000, 20000, 25000, 30000, 40000, 50000, 60000, 80000, 100000};

  /**
   * @brief Find the supported mode matching a resolution and frame rate.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param fps Frame rate.
   * @return The matching mode, or null when the combination is not offered.
   */
  const video_mode_t *find_video_mode(int width, int height, int fps = 60);

  /**
   * @brief Report whether a mode offers a codec.
   * @param mode Supported mode.
   * @param codec Codec to check.
   * @return True when the codec may be selected with the mode.
   */
  bool mode_supports_codec(const video_mode_t &mode, video_codec_e codec);

  /**
   * @brief Report whether a mode offers HDR with a codec.
   * @param mode Supported mode.
   * @param codec Codec to check.
   * @return True when HDR may be selected with the mode and codec.
   */
  bool mode_supports_hdr(const video_mode_t &mode, video_codec_e codec);

  /**
   * @brief Validate settings requested by the TV.
   * @param settings Requested settings.
   * @return A user-safe reason when the settings are rejected, or nothing when they are valid.
   */
  std::optional<std::string> validate_stream_settings(const stream_settings_t &settings);

  /**
   * @brief Return the protocol name of a codec.
   * @param codec Codec to name.
   * @return "h264", "hevc" or "av1".
   */
  std::string_view codec_name(video_codec_e codec);

  /**
   * @brief Parse a protocol codec name.
   * @param name Name sent by the TV.
   * @return The codec, or nothing for an unknown name.
   */
  std::optional<video_codec_e> parse_codec(std::string_view name);

  /**
   * @brief Protocol violation reported to the TV with a stable error code.
   */
  class protocol_error_t: public std::runtime_error {
  public:
    /**
     * @brief Create a protocol error.
     * @param code Stable error code sent to the TV.
     * @param message User-safe description.
     */
    protocol_error_t(std::string code, const std::string &message);

    /**
     * @brief Return the stable error code.
     * @return Error code sent to the TV.
     */
    const std::string &code() const noexcept;

  private:
    std::string _code;  ///< Stable error code.
  };

  /**
   * @brief A paired TV proving it holds its secret.
   */
  struct authenticate_t {
    std::string client_id;  ///< Client ID issued at pairing.
    std::string proof;  ///< HMAC-SHA256 over the connection nonce.
  };

  /**
   * @brief A TV asking to pair, showing a PIN for the user to enter in Sunshine's Web UI.
   */
  struct request_pairing_t {
    std::string pin;  ///< Four-digit PIN the TV shows.
    std::string client_name;  ///< Display name of the TV.
  };

  /**
   * @brief Request for the application list.
   */
  struct get_apps_t {};

  /**
   * @brief Request for one application's artwork.
   */
  struct get_app_artwork_t {
    std::string app_id;  ///< Application ID.
  };

  /**
   * @brief Request to end the stream while leaving the application running.
   */
  struct stop_session_t {};

  /**
   * @brief Request to stop the running application.
   */
  struct stop_host_session_t {};

  /**
   * @brief Request to start or resume a stream.
   */
  struct start_session_t {
    std::string app_id;  ///< Application ID.
    stream_settings_t settings;  ///< Validated stream settings.
  };

  /**
   * @brief Request to stop the running application and start another.
   */
  struct switch_session_t {
    std::string app_id;  ///< Application ID.
    stream_settings_t settings;  ///< Validated stream settings.
  };

  /**
   * @brief SDP answer for a session's offer.
   */
  struct answer_t {
    std::uint64_t session_id = 0;  ///< Session the answer belongs to.
    std::string sdp;  ///< Answer SDP.
  };

  /**
   * @brief Remote ICE candidate.
   */
  struct candidate_t {
    std::uint64_t session_id = 0;  ///< Session the candidate belongs to.
    std::string candidate;  ///< Candidate line.
    std::string mid;  ///< Media ID.
  };

  /**
   * @brief Any message the TV may send.
   */
  using client_payload_t = std::variant<authenticate_t, request_pairing_t, get_apps_t, get_app_artwork_t, start_session_t, stop_session_t, stop_host_session_t, switch_session_t, answer_t, candidate_t>;

  /**
   * @brief A parsed TV message.
   */
  struct client_message_t {
    std::string type;  ///< Message type.
    client_payload_t payload;  ///< Parsed payload.
  };

  /**
   * @brief Server state reported after authentication.
   */
  struct gateway_status_t {
    std::string gateway_name;  ///< Name shown on the TV.
    bool session_active = false;  ///< Whether this TV has a stream.
    std::optional<std::string> running_app_id;  ///< Application running on the host, if any.
    std::optional<std::string> mac_address;  ///< Wake-on-LAN address of the adapter facing the TV.
  };

  /**
   * @brief One entry of the application list.
   */
  struct application_t {
    std::string id;  ///< Opaque application ID.
    std::string title;  ///< Display title.
    bool artwork_available = false;  ///< Whether artwork is already cached.
    bool running = false;  ///< Whether the application is running on the host.
  };

  /**
   * @brief Encoders available beyond H.264 and HEVC.
   */
  struct encoder_support_t {
    bool av1 = false;  ///< AV1 Main 8-bit.
    bool av1_hdr = false;  ///< AV1 Main 10-bit.
  };

  /**
   * @brief Parse a message from the TV.
   * @param text JSON text.
   * @return The parsed message.
   * @throws protocol_error_t for malformed or unsupported messages.
   */
  client_message_t parse_client_message(std::string_view text);

  /**
   * @brief Build the greeting carrying the authentication nonce.
   *
   * It also announces `"pairing": "client-pin"`: an unpaired TV shows its own PIN and sends
   * `request-pairing`, rather than asking for a PIN shown on the PC as the standalone Gateway does.
   *
   * @param nonce Single-use nonce in lower-case hex.
   * @param mac_address Wake-on-LAN address, offered before authentication like ARP does.
   * @param sunshine_available Whether streaming is possible, for the TV's reachability probe.
   * @return The message.
   */
  nlohmann::json make_auth_required(std::string_view nonce, const std::optional<std::string> &mac_address, std::optional<bool> sunshine_available);

  /**
   * @brief Build the push sent when streaming becomes possible or impossible.
   * @param sunshine_available Whether streaming is possible.
   * @return The message.
   */
  nlohmann::json make_sunshine_availability(bool sunshine_available);

  /**
   * @brief Build the reply to a successful authentication.
   * @return The message.
   */
  nlohmann::json make_authenticated();

  /**
   * @brief Build the reply to a successful pairing.
   * @param client_id Issued client ID.
   * @param client_secret Issued client secret.
   * @return The message.
   */
  nlohmann::json make_paired(std::string_view client_id, std::string_view client_secret);

  /**
   * @brief Build the server status message.
   * @param status Server status.
   * @return The message.
   */
  nlohmann::json make_gateway_status(const gateway_status_t &status);

  /**
   * @brief Build the capabilities message listing every selectable mode.
   * @param encoders Encoders available beyond H.264 and HEVC.
   * @return The message.
   */
  nlohmann::json make_capabilities(const encoder_support_t &encoders);

  /**
   * @brief Build the application list.
   * @param applications Applications.
   * @return The message.
   */
  nlohmann::json make_apps(const std::vector<application_t> &applications);

  /**
   * @brief Build the reply to an artwork request.
   * @param app_id Application ID.
   * @param available Whether artwork is included.
   * @param mime_type Artwork media type.
   * @param base64_data Base64 artwork bytes.
   * @return The message.
   */
  nlohmann::json make_app_artwork(std::string_view app_id, bool available, std::string_view mime_type = {}, std::string_view base64_data = {});

  /**
   * @brief Build a stream state change.
   * @param state New state.
   * @param session_id Session the state belongs to.
   * @param settings Session settings.
   * @param message Optional detail.
   * @return The message.
   */
  nlohmann::json make_session_status(std::string_view state, std::optional<std::uint64_t> session_id = std::nullopt, std::optional<stream_settings_t> settings = std::nullopt, std::optional<std::string> message = std::nullopt);

  /**
   * @brief Build a host application state change.
   * @param state New state.
   * @param running_app_id Running application.
   * @param target_app_id Application being switched to.
   * @param message Optional detail.
   * @return The message.
   */
  nlohmann::json make_host_session_status(std::string_view state, std::optional<std::string> running_app_id = std::nullopt, std::optional<std::string> target_app_id = std::nullopt, std::optional<std::string> message = std::nullopt);

  /**
   * @brief Build an error reply.
   * @param request_type Request that failed.
   * @param code Stable error code.
   * @param message User-safe description.
   * @return The message.
   */
  nlohmann::json make_error(std::string_view request_type, std::string_view code, std::string_view message);

  /**
   * @brief Build an SDP offer.
   * @param session_id Session the offer belongs to.
   * @param sdp Offer SDP.
   * @return The message.
   */
  nlohmann::json make_offer(std::uint64_t session_id, std::string_view sdp);

  /**
   * @brief Build a local ICE candidate.
   * @param session_id Session the candidate belongs to.
   * @param candidate Candidate line.
   * @param mid Media ID.
   * @return The message.
   */
  nlohmann::json make_candidate(std::uint64_t session_id, std::string_view candidate, std::string_view mid);
}  // namespace webrtc_stream::protocol
