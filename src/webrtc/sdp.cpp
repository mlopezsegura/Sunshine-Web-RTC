/**
 * @file src/webrtc/sdp.cpp
 * @brief Definitions for the SDP details Samsung Tizen needs for low-latency streaming.
 */
// standard includes
#include <array>
#include <charconv>

// local includes
#include "sdp.h"

namespace webrtc_stream::sdp {
  namespace {
    /**
     * @brief Return the video media section of an SDP.
     * @param sdp SDP text.
     * @return The section, or an empty view when there is none.
     */
    std::string_view video_section(std::string_view sdp) {
      const auto begin = sdp.find("m=video ");
      if (begin == std::string_view::npos) {
        return {};
      }
      const auto end = sdp.find("\r\nm=", begin + 1);
      return sdp.substr(begin, end == std::string_view::npos ? end : end - begin);
    }

    /**
     * @brief Remove leading and trailing spaces.
     * @param value Text to trim.
     * @return The trimmed text.
     */
    std::string_view trim(std::string_view value) {
      while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }
      while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
      }
      return value;
    }

    /**
     * @brief Read one format parameter of a payload type.
     * @param section Media section.
     * @param payload_type RTP payload type.
     * @param name Parameter name.
     * @return The value, or nothing when it is absent.
     */
    std::optional<std::string_view> format_parameter(std::string_view section, int payload_type, std::string_view name) {
      const auto prefix = "a=fmtp:" + std::to_string(payload_type) + " ";
      const auto position = section.find(prefix);
      if (position == std::string_view::npos) {
        return std::nullopt;
      }
      const auto end = section.find("\r\n", position);
      auto parameters = section.substr(position + prefix.size(), end == std::string_view::npos ? end : end - position - prefix.size());
      while (!parameters.empty()) {
        const auto separator = parameters.find(';');
        const auto parameter = trim(parameters.substr(0, separator));
        const auto equals = parameter.find('=');
        if (equals != std::string_view::npos && parameter.substr(0, equals) == name) {
          return parameter.substr(equals + 1);
        }
        if (separator == std::string_view::npos) {
          break;
        }
        parameters.remove_prefix(separator + 1);
      }
      return std::nullopt;
    }

    /**
     * @brief Return the rtpmap encoding name of a codec.
     * @param codec Codec.
     * @return The encoding name and clock rate.
     */
    std::string_view rtpmap_name(protocol::video_codec_e codec) {
      switch (codec) {
        case protocol::video_codec_e::h264:
          return "H264/90000";
        case protocol::video_codec_e::hevc:
          return "H265/90000";
        case protocol::video_codec_e::av1:
          return "AV1/90000";
      }
      return {};
    }
  }  // namespace

  std::string game_mode_image_attribute(const protocol::stream_settings_t &settings, int payload_type) {
    const auto width = std::to_string(settings.width);
    const auto height = std::to_string(settings.height);
    const auto fps = std::to_string(settings.fps);
    return "imageattr:" + std::to_string(payload_type) + " send [x=[" + width + ":" + width + "],y=[" + height + ":" + height + "],fps=[" + fps + ":" + fps + "]]";
  }

  bool has_valid_game_mode_image_attribute(std::string_view sdp, const protocol::stream_settings_t &settings, int payload_type) {
    for (std::size_t index = 0; index < sdp.size(); ++index) {
      const bool bare_lf = sdp[index] == '\n' && (index == 0 || sdp[index - 1] != '\r');
      const bool bare_cr = sdp[index] == '\r' && (index + 1 == sdp.size() || sdp[index + 1] != '\n');
      if (bare_lf || bare_cr) {
        return false;
      }
    }

    const auto expected = "a=" + game_mode_image_attribute(settings, payload_type);
    const auto position = sdp.find(expected);
    if (position == std::string_view::npos || sdp.find(expected, position + expected.size()) != std::string_view::npos || sdp.find("a=imageattr:") != position || sdp.find("a=imageattr:", position + 1) != std::string_view::npos) {
      return false;
    }

    const bool complete_line = (position == 0 || sdp.substr(position - 2, 2) == "\r\n") && sdp.substr(position + expected.size(), 2) == "\r\n";
    const auto video_position = sdp.find("m=video ");
    const auto next_media_position = video_position == std::string_view::npos ? std::string_view::npos : sdp.find("\r\nm=", video_position + 1);
    return complete_line && video_position != std::string_view::npos && position > video_position && (next_media_position == std::string_view::npos || position < next_media_position);
  }

  bool has_expected_video_codec(std::string_view sdp, protocol::video_codec_e codec, int payload_type) {
    const auto section = video_section(sdp);
    if (section.empty()) {
      return false;
    }
    const auto expected = "a=rtpmap:" + std::to_string(payload_type) + " " + std::string(rtpmap_name(codec));
    if (section.find(expected) == std::string_view::npos) {
      return false;
    }
    // The offer must carry the requested codec alone, so the TV cannot answer another one.
    constexpr std::array ALL_CODECS {protocol::video_codec_e::h264, protocol::video_codec_e::hevc, protocol::video_codec_e::av1};
    for (const auto other : ALL_CODECS) {
      if (other != codec && section.find(rtpmap_name(other)) != std::string_view::npos) {
        return false;
      }
    }
    return true;
  }

  std::optional<std::string> hevc_format_parameters(const protocol::stream_settings_t &settings) {
    if (settings.codec != protocol::video_codec_e::hevc || !settings.hdr) {
      return std::nullopt;
    }
    int level_id = 123;
    if (settings.width == 2560) {
      level_id = 150;
    } else if (settings.width == 3840) {
      level_id = 153;
    }
    return "profile-id=2;tier-flag=0;level-id=" + std::to_string(level_id);
  }

  bool has_expected_hevc_format_parameters(std::string_view sdp, const protocol::stream_settings_t &settings, int payload_type) {
    if (!hevc_format_parameters(settings)) {
      return true;
    }
    const auto section = video_section(sdp);
    return !section.empty() && format_parameter(section, payload_type, "profile-id") == "2" && format_parameter(section, payload_type, "tier-flag") == "0";
  }

  std::optional<int> hevc_level_id(std::string_view sdp, int payload_type) {
    const auto value = format_parameter(video_section(sdp), payload_type, "level-id");
    if (!value) {
      return std::nullopt;
    }
    int level = 0;
    const auto parsed = std::from_chars(value->data(), value->data() + value->size(), level);
    if (parsed.ec != std::errc {} || parsed.ptr != value->data() + value->size()) {
      return std::nullopt;
    }
    return level;
  }

  bool has_data_channel_section(std::string_view sdp) {
    const auto application = sdp.find("m=application ");
    return application != std::string_view::npos && sdp.find("webrtc-datachannel", application) != std::string_view::npos && sdp.find("a=sctp-port:", application) != std::string_view::npos;
  }
}  // namespace webrtc_stream::sdp
