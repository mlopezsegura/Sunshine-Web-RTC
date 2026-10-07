/**
 * @file tests/unit/webrtc/test_webrtc_sdp.cpp
 * @brief Tests for the Samsung Tizen SDP helpers.
 */
// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/webrtc/sdp.h"

using namespace webrtc_stream;

namespace {
  protocol::stream_settings_t settings(int width, int height, protocol::video_codec_e codec, bool hdr) {
    protocol::stream_settings_t value;
    value.width = width;
    value.height = height;
    value.codec = codec;
    value.hdr = hdr;
    return value;
  }

  std::string offer(std::string_view video_lines) {
    return "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n" + std::string(video_lines) + "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=sctp-port:5000\r\n";
  }
}  // namespace

TEST(WebrtcSdpTest, BuildsTheGameModeImageAttribute) {
  EXPECT_EQ(sdp::game_mode_image_attribute(settings(1920, 1080, protocol::video_codec_e::h264, false)), "imageattr:96 send [x=[1920:1920],y=[1080:1080],fps=[60:60]]");
}

TEST(WebrtcSdpTest, AcceptsOneImageAttributeInTheVideoSection) {
  const auto value = settings(1920, 1080, protocol::video_codec_e::h264, false);
  const auto attribute = "a=" + sdp::game_mode_image_attribute(value) + "\r\n";
  EXPECT_TRUE(sdp::has_valid_game_mode_image_attribute(offer("a=rtpmap:96 H264/90000\r\n" + attribute), value));
  EXPECT_FALSE(sdp::has_valid_game_mode_image_attribute(offer(attribute + attribute), value));
  EXPECT_FALSE(sdp::has_valid_game_mode_image_attribute(offer("a=rtpmap:96 H264/90000\r\n"), value));
  EXPECT_FALSE(sdp::has_valid_game_mode_image_attribute(offer(attribute) + "a=bare\n", value));
}

TEST(WebrtcSdpTest, RequiresTheRequestedCodecAlone) {
  EXPECT_TRUE(sdp::has_expected_video_codec(offer("a=rtpmap:96 H265/90000\r\n"), protocol::video_codec_e::hevc));
  EXPECT_FALSE(sdp::has_expected_video_codec(offer("a=rtpmap:96 H265/90000\r\na=rtpmap:97 H264/90000\r\n"), protocol::video_codec_e::hevc));
  EXPECT_FALSE(sdp::has_expected_video_codec(offer("a=rtpmap:96 H264/90000\r\n"), protocol::video_codec_e::av1));
  EXPECT_TRUE(sdp::has_expected_video_codec(offer("a=rtpmap:96 AV1/90000\r\n"), protocol::video_codec_e::av1));
}

TEST(WebrtcSdpTest, RequestsHevcMain10AtTheLevelOfTheResolution) {
  EXPECT_FALSE(sdp::hevc_format_parameters(settings(1920, 1080, protocol::video_codec_e::hevc, false)));
  EXPECT_FALSE(sdp::hevc_format_parameters(settings(1920, 1080, protocol::video_codec_e::av1, true)));
  EXPECT_EQ(sdp::hevc_format_parameters(settings(1920, 1080, protocol::video_codec_e::hevc, true)), "profile-id=2;tier-flag=0;level-id=123");
  EXPECT_EQ(sdp::hevc_format_parameters(settings(2560, 1440, protocol::video_codec_e::hevc, true)), "profile-id=2;tier-flag=0;level-id=150");
  EXPECT_EQ(sdp::hevc_format_parameters(settings(3840, 2160, protocol::video_codec_e::hevc, true)), "profile-id=2;tier-flag=0;level-id=153");
}

TEST(WebrtcSdpTest, ChecksThatTheAnswerKeepsMain10) {
  const auto hdr = settings(3840, 2160, protocol::video_codec_e::hevc, true);
  EXPECT_TRUE(sdp::has_expected_hevc_format_parameters(offer("a=fmtp:96 level-id=150; profile-id=2;tier-flag=0\r\n"), hdr));
  EXPECT_FALSE(sdp::has_expected_hevc_format_parameters(offer("a=fmtp:96 profile-id=1;tier-flag=0\r\n"), hdr));
  EXPECT_TRUE(sdp::has_expected_hevc_format_parameters(offer(""), settings(1920, 1080, protocol::video_codec_e::hevc, false)));
  EXPECT_EQ(sdp::hevc_level_id(offer("a=fmtp:96 profile-id=2;level-id=150\r\n")), 150);
  EXPECT_FALSE(sdp::hevc_level_id(offer("a=fmtp:96 profile-id=2;level-id=15x\r\n")));
}

TEST(WebrtcSdpTest, DetectsTheDataChannelSection) {
  EXPECT_TRUE(sdp::has_data_channel_section(offer("")));
  EXPECT_FALSE(sdp::has_data_channel_section("v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"));
}
