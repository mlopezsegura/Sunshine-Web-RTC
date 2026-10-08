/**
 * @file tests/unit/webrtc/test_webrtc_protocol.cpp
 * @brief Tests for the Moonlight WebRTC TV protocol.
 */
// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/webrtc/protocol.h"

using namespace webrtc_stream::protocol;

namespace {
  stream_settings_t settings(int width, int height, video_codec_e codec, bool hdr = false, int bitrate = 20000) {
    stream_settings_t value;
    value.width = width;
    value.height = height;
    value.codec = codec;
    value.hdr = hdr;
    value.bitrate_kbps = bitrate;
    return value;
  }

  const nlohmann::json *find_mode(const nlohmann::json &capabilities, int width) {
    for (const auto &mode : capabilities.at("videoModes")) {
      if (mode.at("width") == width) {
        return &mode;
      }
    }
    return nullptr;
  }

  std::string protocol_error_code(std::string_view text) {
    try {
      parse_client_message(text);
    } catch (const protocol_error_t &e) {
      return e.code();
    }
    return {};
  }
}  // namespace

TEST(WebrtcProtocolTest, AcceptsEverySupportedModeAndCodec) {
  EXPECT_FALSE(validate_stream_settings(settings(1280, 720, video_codec_e::h264)));
  EXPECT_FALSE(validate_stream_settings(settings(1920, 1080, video_codec_e::hevc, true)));
  EXPECT_FALSE(validate_stream_settings(settings(2560, 1440, video_codec_e::av1, true)));
  EXPECT_FALSE(validate_stream_settings(settings(3840, 2160, video_codec_e::hevc, true, 50000)));
}

TEST(WebrtcProtocolTest, RejectsUnsupportedCombinations) {
  EXPECT_EQ(validate_stream_settings(settings(3840, 2160, video_codec_e::h264)), "Unsupported resolution and codec combination");
  EXPECT_TRUE(validate_stream_settings(settings(1280, 720, video_codec_e::hevc, true)));
  EXPECT_TRUE(validate_stream_settings(settings(1920, 1080, video_codec_e::h264, true)));
  EXPECT_EQ(validate_stream_settings(settings(1600, 900, video_codec_e::h264)), "Unsupported resolution");
  EXPECT_EQ(validate_stream_settings(settings(1920, 1080, video_codec_e::h264, false, 21000)), "Unsupported bitrate");

  auto surround = settings(1920, 1080, video_codec_e::h264);
  surround.audio_channels = 6;
  EXPECT_EQ(validate_stream_settings(surround), "Only stereo audio is supported");

  auto fps = settings(1920, 1080, video_codec_e::h264);
  fps.fps = 120;
  EXPECT_FALSE(validate_stream_settings(fps));
  fps.fps = 144;
  EXPECT_EQ(validate_stream_settings(fps), "Unsupported frame rate");
  fps.fps = 75;
  EXPECT_EQ(validate_stream_settings(fps), "Unsupported frame rate");
}

TEST(WebrtcProtocolTest, ParsesStartSession) {
  const auto message = parse_client_message(
    R"({"version":2,"type":"start-session","appId":"7",)"
    R"("video":{"width":3840,"height":2160,"fps":60,"codec":"hevc","bitrateKbps":50000,"hdr":true},)"
    R"("audio":{"channels":2}})"
  );
  const auto &start = std::get<start_session_t>(message.payload);
  EXPECT_EQ(message.type, "start-session");
  EXPECT_EQ(start.app_id, "7");
  EXPECT_EQ(start.settings, settings(3840, 2160, video_codec_e::hevc, true, 50000));
}

TEST(WebrtcProtocolTest, RejectsVersionOneAndMalformedMessages) {
  EXPECT_EQ(protocol_error_code(R"({"version":1,"type":"get-apps"})"), "unsupported-version");
  EXPECT_EQ(protocol_error_code("{"), "invalid-json");
  EXPECT_EQ(protocol_error_code(R"({"version":2,"type":"candidate","sessionId":0,"candidate":"a","mid":"0"})"), "invalid-message");
  EXPECT_EQ(protocol_error_code(R"({"version":2,"type":"get-app-artwork","appId":7})"), "invalid-message");
  EXPECT_EQ(protocol_error_code(R"({"version":2,"type":"reboot"})"), "unsupported-message");
}

TEST(WebrtcProtocolTest, ParsesAuthenticationAndPairing) {
  const auto authenticate = parse_client_message(R"({"version":2,"type":"authenticate","clientId":"ab","proof":"cd"})");
  EXPECT_EQ(std::get<authenticate_t>(authenticate.payload).client_id, "ab");

  const auto pair = parse_client_message(R"({"version":2,"type":"request-pairing","pin":"0421","clientName":"Samsung TV"})");
  EXPECT_EQ(std::get<request_pairing_t>(pair.payload).pin, "0421");
  EXPECT_EQ(std::get<request_pairing_t>(pair.payload).client_name, "Samsung TV");

  EXPECT_EQ(protocol_error_code(R"({"version":2,"type":"request-pairing","pin":"04a1"})"), "invalid-message");
  // TV apps that enter a PIN shown on the PC are told to update.
  EXPECT_EQ(protocol_error_code(R"({"version":2,"type":"pair-client","pin":"0421"})"), "unsupported-pairing");
}

TEST(WebrtcProtocolTest, AnswersTvsLookingForSunshine) {
  EXPECT_TRUE(is_discovery_request(R"({"version":2,"type":"discover"})"));
  EXPECT_FALSE(is_discovery_request(R"({"version":1,"type":"discover"})"));
  EXPECT_FALSE(is_discovery_request(R"({"version":2,"type":"get-apps"})"));
  EXPECT_FALSE(is_discovery_request("discover"));
  EXPECT_FALSE(is_discovery_request(""));

  const auto reply = make_discovery_response("Living Room PC", 8001, std::string("2C:F0:5D:7B:E6:D0"));
  EXPECT_EQ(reply.at("type"), "discovery");
  EXPECT_EQ(reply.at("version"), 2);
  EXPECT_EQ(reply.at("name"), "Living Room PC");
  EXPECT_EQ(reply.at("port"), 8001);
  EXPECT_EQ(reply.at("macAddress"), "2C:F0:5D:7B:E6:D0");
  EXPECT_FALSE(make_discovery_response("PC", 8000, std::nullopt).contains("macAddress"));
}

TEST(WebrtcProtocolTest, AnnouncesThatTheTvShowsThePin) {
  EXPECT_EQ(make_auth_required("00", std::nullopt, true).at("pairing"), "client-pin");
}

TEST(WebrtcProtocolTest, OffersAv1OnlyWhenTheEncoderSupportsIt) {
  const auto without_av1 = make_capabilities({false, false});
  EXPECT_EQ(without_av1.at("codecs"), nlohmann::json::array({"h264", "hevc"}));
  EXPECT_EQ(find_mode(without_av1, 3840)->at("codecs"), nlohmann::json::array({"hevc"}));

  const auto av1_sdr = make_capabilities({true, false});
  EXPECT_EQ(find_mode(av1_sdr, 1920)->at("codecs"), nlohmann::json::array({"h264", "hevc", "av1"}));
  EXPECT_EQ(find_mode(av1_sdr, 1920)->at("hdrCodecs"), nlohmann::json::array({"hevc"}));

  const auto av1_hdr = make_capabilities({true, true});
  EXPECT_EQ(find_mode(av1_hdr, 3840)->at("hdrCodecs"), nlohmann::json::array({"hevc", "av1"}));
  EXPECT_EQ(find_mode(av1_hdr, 1280)->at("hdrCodecs"), nlohmann::json::array());
  EXPECT_EQ(av1_hdr.at("frameRates"), nlohmann::json::array({30, 60, 90, 120}));
  // Older TV apps request the mode's fps; newer ones choose from its frameRates.
  EXPECT_EQ(find_mode(av1_hdr, 3840)->at("fps"), 60);
  EXPECT_EQ(find_mode(av1_hdr, 3840)->at("frameRates"), nlohmann::json::array({30, 60, 90, 120}));
}

TEST(WebrtcProtocolTest, ReportsSunshineAsDetectedAndPaired) {
  gateway_status_t status;
  status.gateway_name = "Gaming-PC";
  status.running_app_id = "7";
  status.mac_address = "2C:F0:5D:7B:E6:D0";
  const auto message = make_gateway_status(status);
  EXPECT_EQ(message.at("version"), 2);
  EXPECT_EQ(message.at("type"), "gateway-status");
  EXPECT_TRUE(message.at("sunshineDetected"));
  EXPECT_TRUE(message.at("sunshinePaired"));
  EXPECT_FALSE(message.at("sessionActive"));
  EXPECT_EQ(message.at("runningAppId"), "7");
  EXPECT_EQ(message.at("macAddress"), "2C:F0:5D:7B:E6:D0");
}

TEST(WebrtcProtocolTest, IncludesSettingsInSessionStatus) {
  const auto message = make_session_status("streaming", 3, settings(1920, 1080, video_codec_e::av1, true));
  EXPECT_EQ(message.at("sessionId"), 3);
  EXPECT_EQ(message.at("video").at("codec"), "av1");
  EXPECT_TRUE(message.at("video").at("hdr"));
  EXPECT_EQ(message.at("audio").at("sampleRate"), 48000);
}
