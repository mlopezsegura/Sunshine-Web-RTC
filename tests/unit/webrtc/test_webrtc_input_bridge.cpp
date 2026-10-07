/**
 * @file tests/unit/webrtc/test_webrtc_input_bridge.cpp
 * @brief Tests for translating TV gamepad messages into Sunshine input.
 */
// standard includes
#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// lib includes
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

// local includes
#include "src/globals.h"
#include "src/input.h"
#include "src/utility.h"
#include "src/webrtc/input_bridge.h"

using namespace webrtc_stream;

namespace {
  /**
   * @brief Records what the bridge emits.
   */
  struct recorder_t {
    struct arrival_t {
      std::uint8_t slot;
      std::uint8_t type;
      std::uint16_t capabilities;
    };

    struct state_t {
      std::uint8_t slot;
      std::uint16_t mask;
      controller_state_t state;
    };

    std::mutex mutex;
    std::vector<arrival_t> arrivals;
    std::vector<state_t> states;
    std::vector<std::pair<bool, std::uint8_t>> mouse_buttons;
    std::vector<std::string> control_messages;

    input_api_t api() {
      input_api_t api;
      api.controller_arrival = [this](std::uint8_t slot, std::uint16_t, std::uint8_t type, std::uint32_t, std::uint16_t capabilities) {
        std::lock_guard lock(mutex);
        arrivals.push_back({slot, type, capabilities});
      };
      api.controller_state = [this](std::uint8_t slot, std::uint16_t mask, const controller_state_t &state) {
        std::lock_guard lock(mutex);
        states.push_back({slot, mask, state});
      };
      api.mouse_move = [](std::int16_t, std::int16_t) {
      };
      api.mouse_button = [this](bool pressed, std::uint8_t button) {
        std::lock_guard lock(mutex);
        mouse_buttons.emplace_back(pressed, button);
      };
      api.scroll = [](std::int8_t) {
      };
      api.horizontal_scroll = [](std::int8_t) {
      };
      return api;
    }

    input_bridge_t::control_sender_t sender() {
      return [this](const std::string &message) {
        std::lock_guard lock(mutex);
        control_messages.push_back(message);
      };
    }
  };

  constexpr auto CONNECTED = R"json({"v":2,"type":"gamepad-connected","controllerId":5,"id":"Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e)","mapping":"standard","buttons":17,"axes":4,"dualRumble":true})json";

  std::string gamepad_state(std::uint64_t sequence, std::uint32_t buttons, double left_stick_y = 0.0, double right_trigger = 0.0) {
    return nlohmann::json {
      {"v", 2},
      {"type", "gamepad-state"},
      {"controllerId", 5},
      {"seq", sequence},
      {"timestampMs", 1},
      {"buttons", buttons},
      {"leftTrigger", 0},
      {"rightTrigger", right_trigger},
      {"leftStickX", 0},
      {"leftStickY", left_stick_y},
      {"rightStickX", 0},
      {"rightStickY", 0},
    }
      .dump();
  }

  template<class T>
  T packet_as(const std::vector<std::uint8_t> &bytes) {
    T packet;
    EXPECT_EQ(bytes.size(), sizeof(T));
    std::memcpy(&packet, bytes.data(), sizeof(T));
    return packet;
  }
}  // namespace

TEST(WebrtcInputBridgeTest, BuildsPacketsSunshineAccepts) {
  std::vector<std::vector<std::uint8_t>> packets;
  auto api = make_packet_input_api([&packets](std::vector<std::uint8_t> &&packet) {
    packets.push_back(std::move(packet));
  });
  controller_state_t state;
  state.button_flags = A_FLAG | SPECIAL_FLAG;
  state.right_trigger = 200;
  state.left_stick_y = -1234;
  api.controller_arrival(2, 0x4, LI_CTYPE_PS, A_FLAG, LI_CCAP_RUMBLE);
  api.controller_state(2, 0x4, state);
  api.mouse_move(3, -4);
  api.mouse_button(true, BUTTON_RIGHT);
  api.scroll(-1);
  api.horizontal_scroll(1);
  ASSERT_EQ(packets.size(), 6);
  for (const auto &packet : packets) {
    EXPECT_TRUE(input::testing::is_valid_input_packet(packet));
  }

  const auto arrival = packet_as<SS_CONTROLLER_ARRIVAL_PACKET>(packets[0]);
  EXPECT_EQ(util::endian::little(arrival.header.magic), SS_CONTROLLER_ARRIVAL_MAGIC);
  EXPECT_EQ(arrival.controllerNumber, 2);
  EXPECT_EQ(arrival.type, LI_CTYPE_PS);
  EXPECT_EQ(util::endian::little(arrival.capabilities), LI_CCAP_RUMBLE);

  const auto controller = packet_as<NV_MULTI_CONTROLLER_PACKET>(packets[1]);
  EXPECT_EQ(util::endian::little(controller.header.magic), MULTI_CONTROLLER_MAGIC_GEN5);
  EXPECT_EQ(util::endian::little(controller.activeGamepadMask), 0x4);
  EXPECT_EQ(util::endian::little(controller.buttonFlags), A_FLAG | SPECIAL_FLAG);
  EXPECT_EQ(controller.rightTrigger, 200);
  EXPECT_EQ(util::endian::little(controller.leftStickY), -1234);

  const auto mouse = packet_as<NV_REL_MOUSE_MOVE_PACKET>(packets[2]);
  EXPECT_EQ(util::endian::big(mouse.deltaX), 3);
  EXPECT_EQ(util::endian::big(mouse.deltaY), -4);

  const auto button = packet_as<NV_MOUSE_BUTTON_PACKET>(packets[3]);
  EXPECT_EQ(util::endian::little(button.header.magic), MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
  EXPECT_EQ(button.button, BUTTON_RIGHT);

  EXPECT_EQ(util::endian::big(packet_as<NV_SCROLL_PACKET>(packets[4]).scrollAmt1), -120);
  EXPECT_EQ(util::endian::big(packet_as<SS_HSCROLL_PACKET>(packets[5]).scrollAmount), 120);
}

TEST(WebrtcInputBridgeTest, AnnouncesControllersOnceTheStreamIsActive) {
  recorder_t recorder;
  input_bridge_t bridge(recorder.api(), recorder.sender());
  ASSERT_TRUE(bridge.handle_control_message(CONNECTED));
  EXPECT_TRUE(recorder.arrivals.empty());

  bridge.set_stream_active(true);
  ASSERT_EQ(recorder.arrivals.size(), 1);
  EXPECT_EQ(recorder.arrivals[0].slot, 0);
  EXPECT_EQ(recorder.arrivals[0].type, LI_CTYPE_XBOX);
  EXPECT_EQ(recorder.arrivals[0].capabilities, LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);

  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(1, static_cast<std::uint32_t>(gamepad_button_e::a), 1.0, 0.5)));
  ASSERT_EQ(recorder.states.size(), 1);
  EXPECT_EQ(recorder.states[0].mask, 0x1);
  EXPECT_EQ(recorder.states[0].state.button_flags, A_FLAG);
  // The Gamepad API reports down as positive; Moonlight expects up as positive.
  EXPECT_EQ(recorder.states[0].state.left_stick_y, -32767);
  EXPECT_EQ(recorder.states[0].state.right_trigger, 128);
}

TEST(WebrtcInputBridgeTest, DropsStaleSnapshots) {
  recorder_t recorder;
  input_bridge_t bridge(recorder.api(), recorder.sender());
  bridge.set_stream_active(true);
  ASSERT_TRUE(bridge.handle_control_message(CONNECTED));
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(5, 0)));
  EXPECT_FALSE(bridge.handle_gamepad_message(gamepad_state(4, 1)));
  EXPECT_FALSE(bridge.handle_gamepad_message(gamepad_state(5, 1)));
  EXPECT_FALSE(bridge.handle_gamepad_message(R"({"v":2,"type":"gamepad-state","controllerId":5})"));
  EXPECT_EQ(recorder.states.size(), 1);
}

TEST(WebrtcInputBridgeTest, TogglesMouseModeWithALongStartPress) {
  recorder_t recorder;
  input_bridge_t bridge(recorder.api(), recorder.sender());
  bridge.set_stream_active(true);
  ASSERT_TRUE(bridge.handle_control_message(CONNECTED));

  const auto start = input_bridge_t::clock::now();
  const auto start_button = static_cast<std::uint32_t>(gamepad_button_e::start);
  const auto a_button = static_cast<std::uint32_t>(gamepad_button_e::a);
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(1, start_button), start));
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(2, 0), start + MOUSE_EMULATION_LONG_PRESS_TIME + std::chrono::milliseconds(1)));
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(3, a_button), start + std::chrono::seconds(2)));
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(4, 0), start + std::chrono::seconds(3)));

  ASSERT_EQ(recorder.mouse_buttons.size(), 2);
  EXPECT_EQ(recorder.mouse_buttons[0], std::make_pair(true, static_cast<std::uint8_t>(BUTTON_LEFT)));
  EXPECT_EQ(recorder.mouse_buttons[1], std::make_pair(false, static_cast<std::uint8_t>(BUTTON_LEFT)));
}

TEST(WebrtcInputBridgeTest, NeutralizesControllersWhenTheTransportCloses) {
  recorder_t recorder;
  input_bridge_t bridge(recorder.api(), recorder.sender());
  bridge.set_stream_active(true);
  ASSERT_TRUE(bridge.handle_control_message(CONNECTED));
  ASSERT_TRUE(bridge.handle_gamepad_message(gamepad_state(1, static_cast<std::uint32_t>(gamepad_button_e::b))));
  bridge.on_transport_closed();

  ASSERT_EQ(recorder.states.size(), 3);
  EXPECT_EQ(recorder.states[1].state.button_flags, 0);
  EXPECT_EQ(recorder.states[1].mask, 0x1);
  EXPECT_EQ(recorder.states[2].mask, 0x0);
  EXPECT_FALSE(bridge.handle_gamepad_message(gamepad_state(2, 0)));
}

TEST(WebrtcInputBridgeTest, RelaysRumbleOnlyToControllersThatCanRumble) {
  recorder_t recorder;
  {
    input_bridge_t bridge(recorder.api(), recorder.sender());
    bridge.set_stream_active(true);
    ASSERT_TRUE(bridge.handle_control_message(CONNECTED));
    bridge.handle_rumble(0, 65535, 0);
    bridge.handle_trigger_rumble(0, 65535, 65535);
    bridge.handle_rumble(7, 65535, 65535);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::lock_guard lock(recorder.mutex);
  const auto rumble = std::ranges::count_if(recorder.control_messages, [](const std::string &message) {
    return message.find(R"("type":"rumble")") != std::string::npos && message.find(R"("strongMagnitude":1.0)") != std::string::npos;
  });
  EXPECT_EQ(rumble, 1);
}

TEST(WebrtcInputBridgeTest, DetectsControllerTypes) {
  EXPECT_EQ(input_bridge_t::detect_controller_type("DualSense Wireless Controller (Vendor: 054c Product: 0ce6)"), LI_CTYPE_PS);
  EXPECT_EQ(input_bridge_t::detect_controller_type("Pro Controller (Vendor: 057e)"), LI_CTYPE_NINTENDO);
  EXPECT_EQ(input_bridge_t::detect_controller_type("Generic USB Joystick"), LI_CTYPE_UNKNOWN);
}

TEST(WebrtcPacketRoutingTest, DeliversToTheSessionQueueWhenItRegisteredOne) {
  auto session_mail = std::make_shared<safe::mail_raw_t>();
  {
    auto global = mail::packet_queue<int>(session_mail, "webrtc_test_packets");
    EXPECT_EQ(global, mail::man->queue<int>("webrtc_test_packets"));
  }
  auto own = session_mail->queue<int>("webrtc_test_packets");
  EXPECT_EQ(mail::packet_queue<int>(session_mail, "webrtc_test_packets"), own);
}
