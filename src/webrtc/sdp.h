/**
 * @file src/webrtc/sdp.h
 * @brief Declarations for the SDP details Samsung Tizen needs for low-latency streaming.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>

// local includes
#include "protocol.h"

namespace webrtc_stream::sdp {
  constexpr int VIDEO_PAYLOAD_TYPE = 96;  ///< RTP payload type of the video track.
  constexpr int AUDIO_PAYLOAD_TYPE = 111;  ///< RTP payload type of the audio track.

  /**
   * @brief Build the imageattr that puts the Tizen decoder in Samsung Game Mode.
   * @param settings Stream settings.
   * @param payload_type Video payload type.
   * @return Attribute value without the leading "a=".
   */
  std::string game_mode_image_attribute(const protocol::stream_settings_t &settings, int payload_type = VIDEO_PAYLOAD_TYPE);

  /**
   * @brief Check that an offer carries exactly one well-formed Game Mode imageattr in its video section.
   * @param sdp Offer SDP.
   * @param settings Stream settings.
   * @param payload_type Video payload type.
   * @return True when the attribute is present once and well-formed.
   */
  bool has_valid_game_mode_image_attribute(std::string_view sdp, const protocol::stream_settings_t &settings, int payload_type = VIDEO_PAYLOAD_TYPE);

  /**
   * @brief Check that the video section offers only the requested codec.
   * @param sdp Offer SDP.
   * @param codec Requested codec.
   * @param payload_type Video payload type.
   * @return True when the requested codec is the only one offered.
   */
  bool has_expected_video_codec(std::string_view sdp, protocol::video_codec_e codec, int payload_type = VIDEO_PAYLOAD_TYPE);

  /**
   * @brief Build the H.265 format parameters that request Main10 for HDR.
   * @param settings Stream settings.
   * @return The parameters for HEVC HDR, or nothing otherwise.
   */
  std::optional<std::string> hevc_format_parameters(const protocol::stream_settings_t &settings);

  /**
   * @brief Check that an SDP keeps the requested HEVC Main10 profile.
   * @param sdp Offer or answer SDP.
   * @param settings Stream settings.
   * @param payload_type Video payload type.
   * @return True when no profile was requested or the SDP keeps it.
   */
  bool has_expected_hevc_format_parameters(std::string_view sdp, const protocol::stream_settings_t &settings, int payload_type = VIDEO_PAYLOAD_TYPE);

  /**
   * @brief Read the HEVC level-id of the video section.
   * @param sdp Offer or answer SDP.
   * @param payload_type Video payload type.
   * @return The level-id, or nothing when it is absent.
   */
  std::optional<int> hevc_level_id(std::string_view sdp, int payload_type = VIDEO_PAYLOAD_TYPE);

  /**
   * @brief Check that the SDP has a data-channel application section.
   * @param sdp Offer SDP.
   * @return True when the section is present.
   */
  bool has_data_channel_section(std::string_view sdp);
}  // namespace webrtc_stream::sdp
