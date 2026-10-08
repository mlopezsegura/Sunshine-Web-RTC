/**
 * @file src/webrtc/protocol.cpp
 * @brief Definitions for the Moonlight WebRTC TV protocol (version 2).
 */
// standard includes
#include <algorithm>
#include <cctype>

// local includes
#include "protocol.h"

namespace webrtc_stream::protocol {
  namespace {
    using json = nlohmann::json;

    /**
     * @brief Every codec, in the order the TV lists them.
     */
    constexpr std::array ALL_CODECS {video_codec_e::h264, video_codec_e::hevc, video_codec_e::av1};

    /**
     * @brief Start a message with the protocol version and type.
     * @param type Message type.
     * @return The message.
     */
    json envelope(std::string_view type) {
      return {{"version", VERSION}, {"type", type}};
    }

    /**
     * @brief Reject a message that is not protocol version 2.
     * @param message Parsed message.
     */
    void require_version(const json &message) {
      if (!message.contains("version") || !message.at("version").is_number_integer() || message.at("version").get<int>() != VERSION) {
        throw protocol_error_t("unsupported-version", "Protocol version 2 is required");
      }
    }

    /**
     * @brief Read a positive session ID.
     * @param message Parsed message.
     * @return The session ID.
     */
    std::uint64_t session_id(const json &message) {
      if (!message.contains("sessionId") || !message.at("sessionId").is_number_unsigned()) {
        throw protocol_error_t("invalid-message", "A positive sessionId is required");
      }
      const auto value = message.at("sessionId").get<std::uint64_t>();
      if (value == 0) {
        throw protocol_error_t("invalid-message", "A positive sessionId is required");
      }
      return value;
    }

    /**
     * @brief Read a non-empty application ID string.
     * @param message Parsed message.
     * @return The application ID.
     */
    std::string app_id(const json &message) {
      if (!message.at("appId").is_string()) {
        throw protocol_error_t("invalid-message", "appId must be a string");
      }
      auto value = message.at("appId").get<std::string>();
      if (value.empty()) {
        throw protocol_error_t("invalid-message", "appId must not be empty");
      }
      return value;
    }

    /**
     * @brief Read and validate the settings of a start or switch request.
     * @param message Parsed message.
     * @return Validated settings.
     */
    stream_settings_t parse_settings(const json &message) {
      try {
        const auto &video = message.at("video");
        const auto &audio = message.at("audio");
        const auto codec = parse_codec(video.at("codec").get<std::string>());
        if (!codec) {
          throw protocol_error_t("unsupported-settings", "Unsupported video codec");
        }

        stream_settings_t settings;
        settings.width = video.at("width").get<int>();
        settings.height = video.at("height").get<int>();
        settings.fps = video.at("fps").get<int>();
        settings.codec = *codec;
        settings.bitrate_kbps = video.at("bitrateKbps").get<int>();
        settings.hdr = video.at("hdr").get<bool>();
        settings.audio_channels = audio.at("channels").get<int>();
        if (const auto error = validate_stream_settings(settings)) {
          throw protocol_error_t("unsupported-settings", *error);
        }
        return settings;
      } catch (const protocol_error_t &) {
        throw;
      } catch (const std::exception &) {
        throw protocol_error_t("invalid-message", "Invalid start-session settings");
      }
    }

    /**
     * @brief Describe stream settings as the TV expects them.
     * @param settings Stream settings.
     * @return The video and audio members.
     */
    json settings_json(const stream_settings_t &settings) {
      return {
        {"video",
         {{"width", settings.width},
          {"height", settings.height},
          {"fps", settings.fps},
          {"codec", codec_name(settings.codec)},
          {"bitrateKbps", settings.bitrate_kbps},
          {"hdr", settings.hdr}}},
        {"audio", {{"channels", settings.audio_channels}, {"sampleRate", 48000}}},
      };
    }

    /**
     * @brief List the frame rates a mode offers.
     * @param mode Supported mode.
     * @return The frame rates, lowest first.
     */
    json frame_rates(const video_mode_t &mode) {
      json rates = json::array();
      for (const int rate : SUPPORTED_FRAME_RATES) {
        if (rate <= mode.max_fps) {
          rates.push_back(rate);
        }
      }
      return rates;
    }

    /**
     * @brief Describe one selectable mode, offering only what the encoder supports.
     * @param mode Supported mode.
     * @param encoders Encoders available beyond H.264 and HEVC.
     * @return The mode.
     */
    json video_mode_json(const video_mode_t &mode, const encoder_support_t &encoders) {
      json codecs = json::array();
      json hdr_codecs = json::array();
      for (const auto codec : ALL_CODECS) {
        if (!mode_supports_codec(mode, codec) || (codec == video_codec_e::av1 && !encoders.av1)) {
          continue;
        }
        codecs.push_back(codec_name(codec));
        if (mode_supports_hdr(mode, codec) && (codec != video_codec_e::av1 || encoders.av1_hdr)) {
          hdr_codecs.push_back(codec_name(codec));
        }
      }
      return {
        {"width", mode.width},
        {"height", mode.height},
        // Older TV apps read fps and request exactly it; newer ones choose from frameRates.
        {"fps", DEFAULT_FRAME_RATE},
        {"frameRates", frame_rates(mode)},
        {"codecs", std::move(codecs)},
        {"hdrCodecs", std::move(hdr_codecs)},
        {"defaultCodec", codec_name(mode.default_codec)},
        {"defaultBitrateKbps", mode.default_bitrate_kbps},
        {"experimental", mode.experimental},
        {"hdrSupported", mode.supports_hdr},
        {"hdrExperimental", mode.supports_hdr},
      };
    }
  }  // namespace

  const video_mode_t *find_video_mode(int width, int height, int fps) {
    const auto mode = std::ranges::find_if(SUPPORTED_VIDEO_MODES, [=](const video_mode_t &candidate) {
      return candidate.width == width && candidate.height == height;
    });
    const bool offered_rate = std::ranges::find(SUPPORTED_FRAME_RATES, fps) != SUPPORTED_FRAME_RATES.end();
    return mode == SUPPORTED_VIDEO_MODES.end() || !offered_rate || fps > mode->max_fps ? nullptr : &*mode;
  }

  bool mode_supports_codec(const video_mode_t &mode, video_codec_e codec) {
    switch (codec) {
      case video_codec_e::h264:
        return mode.supports_h264;
      case video_codec_e::hevc:
        return mode.supports_hevc;
      case video_codec_e::av1:
        return mode.supports_av1;
    }
    return false;
  }

  bool mode_supports_hdr(const video_mode_t &mode, video_codec_e codec) {
    return mode.supports_hdr && (codec == video_codec_e::hevc || codec == video_codec_e::av1);
  }

  std::optional<std::string> validate_stream_settings(const stream_settings_t &settings) {
    const auto *mode = find_video_mode(settings.width, settings.height, settings.fps);
    if (!mode) {
      return find_video_mode(settings.width, settings.height, DEFAULT_FRAME_RATE) ? "Unsupported frame rate" : "Unsupported resolution";
    }
    if (!mode_supports_codec(*mode, settings.codec)) {
      return "Unsupported resolution and codec combination";
    }
    if (settings.hdr && !mode_supports_hdr(*mode, settings.codec)) {
      return "HDR is supported only with HEVC or AV1 at 1080p, 1440p, or 4K";
    }
    if (settings.audio_channels != 2) {
      return "Only stereo audio is supported";
    }
    if (std::ranges::find(SUPPORTED_BITRATES_KBPS, settings.bitrate_kbps) == SUPPORTED_BITRATES_KBPS.end()) {
      return "Unsupported bitrate";
    }
    return std::nullopt;
  }

  std::string_view codec_name(video_codec_e codec) {
    switch (codec) {
      case video_codec_e::h264:
        return "h264";
      case video_codec_e::hevc:
        return "hevc";
      case video_codec_e::av1:
        return "av1";
    }
    return "unknown";
  }

  std::optional<video_codec_e> parse_codec(std::string_view name) {
    std::string lowercase(name);
    std::ranges::transform(lowercase, lowercase.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    for (const auto codec : ALL_CODECS) {
      if (lowercase == codec_name(codec)) {
        return codec;
      }
    }
    return std::nullopt;
  }

  protocol_error_t::protocol_error_t(std::string code, const std::string &message):
      std::runtime_error(message),
      _code(std::move(code)) {
  }

  const std::string &protocol_error_t::code() const noexcept {
    return _code;
  }

  client_message_t parse_client_message(std::string_view text) {
    json message;
    try {
      message = json::parse(text);
    } catch (const std::exception &) {
      throw protocol_error_t("invalid-json", "Message is not valid JSON");
    }

    try {
      require_version(message);
      const auto type = message.at("type").get<std::string>();
      if (type == "authenticate") {
        auto client_id = message.at("clientId").get<std::string>();
        auto proof = message.at("proof").get<std::string>();
        if (client_id.empty() || client_id.size() > 64 || proof.empty() || proof.size() > 128) {
          throw protocol_error_t("invalid-message", "clientId and proof are required");
        }
        return {type, authenticate_t {std::move(client_id), std::move(proof)}};
      }
      if (type == "request-pairing") {
        auto pin = message.at("pin").get<std::string>();
        if (pin.size() != 4 || !std::ranges::all_of(pin, [](unsigned char digit) {
              return std::isdigit(digit);
            })) {
          throw protocol_error_t("invalid-message", "pin must have four digits");
        }
        std::string client_name;
        if (message.contains("clientName")) {
          client_name = message.at("clientName").get<std::string>();
        }
        return {type, request_pairing_t {std::move(pin), std::move(client_name)}};
      }
      if (type == "pair-client") {
        // TV apps from before Sunshine took over the Gateway entered a PIN shown on the PC.
        throw protocol_error_t("unsupported-pairing", "Update the TV app: Sunshine pairs TVs with a PIN shown on the TV");
      }
      if (type == "get-apps") {
        return {type, get_apps_t {}};
      }
      if (type == "get-app-artwork") {
        return {type, get_app_artwork_t {app_id(message)}};
      }
      if (type == "stop-session") {
        return {type, stop_session_t {}};
      }
      if (type == "stop-host-session") {
        return {type, stop_host_session_t {}};
      }
      if (type == "start-session") {
        return {type, start_session_t {app_id(message), parse_settings(message)}};
      }
      if (type == "switch-session") {
        return {type, switch_session_t {app_id(message), parse_settings(message)}};
      }
      if (type == "answer") {
        return {type, answer_t {session_id(message), message.at("sdp").get<std::string>()}};
      }
      if (type == "candidate") {
        return {type, candidate_t {session_id(message), message.at("candidate").get<std::string>(), message.at("mid").get<std::string>()}};
      }
      throw protocol_error_t("unsupported-message", "Unsupported message type: " + type);
    } catch (const protocol_error_t &) {
      throw;
    } catch (const std::exception &) {
      throw protocol_error_t("invalid-message", "Message fields are invalid");
    }
  }

  json make_auth_required(std::string_view nonce, const std::optional<std::string> &mac_address, std::optional<bool> sunshine_available) {
    auto message = envelope("auth-required");
    message["nonce"] = nonce;
    message["pairing"] = "client-pin";
    if (mac_address) {
      message["macAddress"] = *mac_address;
    }
    if (sunshine_available) {
      message["sunshineAvailable"] = *sunshine_available;
    }
    return message;
  }

  json make_sunshine_availability(bool sunshine_available) {
    auto message = envelope("sunshine-availability");
    message["sunshineAvailable"] = sunshine_available;
    return message;
  }

  json make_authenticated() {
    return envelope("authenticated");
  }

  json make_paired(std::string_view client_id, std::string_view client_secret) {
    auto message = envelope("paired");
    message.update({{"clientId", client_id}, {"clientSecret", client_secret}});
    return message;
  }

  json make_gateway_status(const gateway_status_t &status) {
    auto message = envelope("gateway-status");
    // Sunshine is the server itself, so it is always detected and needs no Moonlight pairing.
    message.update({{"gatewayName", status.gateway_name}, {"sunshineDetected", true}, {"sunshinePaired", true}, {"sessionActive", status.session_active}});
    if (status.running_app_id) {
      message["runningAppId"] = *status.running_app_id;
    }
    if (status.mac_address) {
      message["macAddress"] = *status.mac_address;
    }
    return message;
  }

  json make_capabilities(const encoder_support_t &encoders) {
    auto message = envelope("capabilities");
    message["videoModes"] = json::array();
    message["resolutions"] = json::array();
    for (const auto &mode : SUPPORTED_VIDEO_MODES) {
      message["videoModes"].push_back(video_mode_json(mode, encoders));
      message["resolutions"].push_back({{"width", mode.width}, {"height", mode.height}, {"experimental", mode.experimental}});
    }
    message.update({
      {"frameRates", SUPPORTED_FRAME_RATES},
      {"codecs", encoders.av1 ? json::array({"h264", "hevc", "av1"}) : json::array({"h264", "hevc"})},
      {"hdr", true},
      {"audio", "stereo"},
      {"audioSampleRate", 48000},
      {"bitratesKbps", SUPPORTED_BITRATES_KBPS},
      {"defaults", {{"720p60", 12000}, {"1080p60", 20000}, {"1440p60", 30000}, {"2160p60", 50000}}},
    });
    return message;
  }

  json make_apps(const std::vector<application_t> &applications) {
    auto message = envelope("apps");
    message["apps"] = json::array();
    for (const auto &application : applications) {
      message["apps"].push_back({{"id", application.id}, {"title", application.title}, {"artworkAvailable", application.artwork_available}, {"running", application.running}});
    }
    return message;
  }

  json make_app_artwork(std::string_view app_id, bool available, std::string_view mime_type, std::string_view base64_data) {
    auto message = envelope("app-artwork");
    message.update({{"appId", app_id}, {"available", available}});
    if (available) {
      message.update({{"mimeType", mime_type}, {"data", base64_data}});
    }
    return message;
  }

  json make_session_status(std::string_view state, std::optional<std::uint64_t> session_id_value, std::optional<stream_settings_t> settings, std::optional<std::string> detail) {
    auto message = envelope("session-status");
    message["state"] = state;
    if (session_id_value) {
      message["sessionId"] = *session_id_value;
    }
    if (settings) {
      message.update(settings_json(*settings));
    }
    if (detail) {
      message["message"] = *detail;
    }
    return message;
  }

  json make_host_session_status(std::string_view state, std::optional<std::string> running_app_id, std::optional<std::string> target_app_id, std::optional<std::string> detail) {
    auto message = envelope("host-session-status");
    message["state"] = state;
    if (running_app_id) {
      message["runningAppId"] = *running_app_id;
    }
    if (target_app_id) {
      message["targetAppId"] = *target_app_id;
    }
    if (detail) {
      message["message"] = *detail;
    }
    return message;
  }

  json make_error(std::string_view request_type, std::string_view code, std::string_view detail) {
    auto message = envelope("error");
    message.update({{"requestType", request_type}, {"code", code}, {"message", detail}});
    return message;
  }

  json make_offer(std::uint64_t session_id_value, std::string_view sdp) {
    auto message = envelope("offer");
    message.update({{"sessionId", session_id_value}, {"sdp", sdp}});
    return message;
  }

  json make_candidate(std::uint64_t session_id_value, std::string_view candidate, std::string_view mid) {
    auto message = envelope("candidate");
    message.update({{"sessionId", session_id_value}, {"candidate", candidate}, {"mid", mid}});
    return message;
  }
}  // namespace webrtc_stream::protocol
