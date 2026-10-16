/**
 * @file tests/unit/webrtc/test_webrtc_stream_protocol.cpp
 * @brief Tests for choosing the streaming protocols and the WebRTC media ports.
 */
// standard includes
#include <filesystem>
#include <string>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/config.h"
#include "src/file_handler.h"
#include "src/webrtc/webrtc_stream.h"

namespace {
  /**
   * @brief Restores the settings a test changes and points the applications file at a temporary one.
   */
  class WebrtcStreamProtocolTest: public testing::Test {
  protected:
    void SetUp() override {
      saved = config::webrtc;
      saved_apps_file = config::stream.file_apps;
      apps_file = std::filesystem::temp_directory_path() / "sunshine_test_webrtc_protocol_apps.json";  // NOSONAR(cpp:S5443): safe for tests
      ASSERT_EQ(file_handler::write_file(apps_file.string().c_str(), "{}"), 0);
      config::stream.file_apps = apps_file.string();
      config::webrtc.protocol = config::stream_protocol_e::both;
    }

    void TearDown() override {
      config::webrtc = saved;
      config::stream.file_apps = saved_apps_file;
      std::filesystem::remove(apps_file);
    }

    config::webrtc_t saved {};  ///< Settings before the test.
    std::string saved_apps_file;  ///< Applications file before the test.
    std::filesystem::path apps_file;  ///< Temporary applications file.
  };
}  // namespace

TEST_F(WebrtcStreamProtocolTest, ParsesEveryProtocol) {
  EXPECT_EQ(config::stream_protocol_from_view("moonlight"), config::stream_protocol_e::moonlight);
  EXPECT_EQ(config::stream_protocol_from_view("webrtc"), config::stream_protocol_e::webrtc);
  EXPECT_EQ(config::stream_protocol_from_view("both"), config::stream_protocol_e::both);
}

TEST_F(WebrtcStreamProtocolTest, ServesBothForAnUnknownProtocol) {
  EXPECT_EQ(config::stream_protocol_from_view("gamestream"), config::stream_protocol_e::both);
}

TEST_F(WebrtcStreamProtocolTest, EnablesTheServersOfTheChosenProtocol) {
  config::webrtc.protocol = config::stream_protocol_e::moonlight;
  EXPECT_TRUE(config::moonlight_enabled());
  EXPECT_FALSE(config::webrtc_enabled());

  config::webrtc.protocol = config::stream_protocol_e::webrtc;
  EXPECT_FALSE(config::moonlight_enabled());
  EXPECT_TRUE(config::webrtc_enabled());

  config::webrtc.protocol = config::stream_protocol_e::both;
  EXPECT_TRUE(config::moonlight_enabled());
  EXPECT_TRUE(config::webrtc_enabled());
}

TEST_F(WebrtcStreamProtocolTest, ReadsTheProtocolFromTheConfigFile) {
  config::apply_config_for_test("stream_protocol = webrtc\n");
  EXPECT_EQ(config::webrtc.protocol, config::stream_protocol_e::webrtc);

  config::apply_config_for_test("stream_protocol = moonlight\n");
  EXPECT_EQ(config::webrtc.protocol, config::stream_protocol_e::moonlight);
}

TEST_F(WebrtcStreamProtocolTest, TreatsADisabledLegacyWebrtcServerAsMoonlightOnly) {
  config::apply_config_for_test("webrtc_enabled = disabled\n");
  EXPECT_EQ(config::webrtc.protocol, config::stream_protocol_e::moonlight);
}

TEST_F(WebrtcStreamProtocolTest, KeepsTheProtocolForAnEnabledLegacyWebrtcServer) {
  config::apply_config_for_test("webrtc_enabled = enabled\n");
  EXPECT_EQ(config::webrtc.protocol, config::stream_protocol_e::both);
}

TEST_F(WebrtcStreamProtocolTest, PrefersTheProtocolOverTheLegacyWebrtcServerOption) {
  config::apply_config_for_test("webrtc_enabled = disabled\nstream_protocol = webrtc\n");
  EXPECT_EQ(config::webrtc.protocol, config::stream_protocol_e::webrtc);
}

TEST_F(WebrtcStreamProtocolTest, ReadsTheMediaPortsFromTheConfigFile) {
  config::apply_config_for_test("webrtc_media_port_min = 40000\nwebrtc_media_port_max = 40019\n");
  EXPECT_EQ(config::webrtc.media_port_min, 40000);
  EXPECT_EQ(config::webrtc.media_port_max, 40019);
}

TEST(WebrtcMediaPortRangeTest, UsesAnyPortByDefault) {
  EXPECT_FALSE(webrtc_stream::media_port_range(0, 0));
}

TEST(WebrtcMediaPortRangeTest, UsesTheConfiguredRange) {
  const auto range = webrtc_stream::media_port_range(40000, 40019);
  ASSERT_TRUE(range);
  EXPECT_EQ(range->first, 40000);
  EXPECT_EQ(range->second, 40019);
}

TEST(WebrtcMediaPortRangeTest, OpensAnEndLeftAtZero) {
  using range_t = std::pair<std::uint16_t, std::uint16_t>;
  EXPECT_EQ(webrtc_stream::media_port_range(0, 40019), range_t(1024, 40019));
  EXPECT_EQ(webrtc_stream::media_port_range(40000, 0), range_t(40000, 65535));
}

TEST(WebrtcMediaPortRangeTest, UsesAnyPortForAnEmptyRange) {
  EXPECT_FALSE(webrtc_stream::media_port_range(40019, 40000));
  EXPECT_FALSE(webrtc_stream::media_port_range(0, 1000));
}
