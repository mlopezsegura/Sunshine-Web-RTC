/**
 * @file src/webrtc/input_bridge.cpp
 * @brief Definitions for translating TV gamepad messages into Sunshine input.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// lib includes
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
#include <nlohmann/json.hpp>

// local includes
#include "input_bridge.h"
#include "src/logging.h"
#include "src/utility.h"

namespace webrtc_stream {
  namespace {
    using json = nlohmann::json;
    using namespace std::literals;

    /**
     * @brief Every bit the TV may set in its button mask.
     */
    constexpr std::uint32_t ALL_PROTOCOL_BUTTONS = (1U << 15) - 1;
    constexpr int WHEEL_DELTA_UNITS = 120;  ///< One wheel click in Moonlight scroll units.

    /**
     * @brief Return the bit of a button.
     * @param button Button.
     * @return Its bit in the TV button mask.
     */
    constexpr std::uint32_t mask(gamepad_button_e button) {
      return static_cast<std::uint32_t>(button);
    }

    /**
     * @brief Buttons that act as mouse buttons in mouse mode.
     */
    constexpr std::array MOUSE_BUTTONS {gamepad_button_e::a, gamepad_button_e::b, gamepad_button_e::x, gamepad_button_e::left_bumper, gamepad_button_e::right_bumper};
    /**
     * @brief Bits of the buttons that act as mouse buttons.
     */
    constexpr std::uint32_t MOUSE_BUTTON_MASK = mask(gamepad_button_e::a) | mask(gamepad_button_e::b) | mask(gamepad_button_e::x) | mask(gamepad_button_e::left_bumper) | mask(gamepad_button_e::right_bumper);

    /**
     * @brief Check whether a button is pressed.
     * @param flags TV button mask.
     * @param button Button.
     * @return True when it is pressed.
     */
    bool has_button(std::uint32_t flags, gamepad_button_e button) {
      return (flags & mask(button)) != 0;
    }

    /**
     * @brief Return the mouse button a gamepad button stands for in mouse mode.
     * @param button Gamepad button.
     * @return The Moonlight mouse button, or 0.
     */
    std::uint8_t moonlight_mouse_button(gamepad_button_e button) {
      switch (button) {
        case gamepad_button_e::a:
          return BUTTON_LEFT;
        case gamepad_button_e::b:
          return BUTTON_RIGHT;
        case gamepad_button_e::x:
          return BUTTON_MIDDLE;
        case gamepad_button_e::left_bumper:
          return BUTTON_X1;
        case gamepad_button_e::right_bumper:
          return BUTTON_X2;
        default:
          return 0;
      }
    }

    /**
     * @brief Check the version and type of a gamepad message.
     * @param message Parsed message.
     * @param expected_type Expected type.
     * @return True when both match.
     */
    bool has_envelope(const json &message, std::string_view expected_type) {
      return message.is_object() && message.contains("v") && message["v"].is_number_unsigned() && message["v"].get<std::uint32_t>() == GAMEPAD_PROTOCOL_VERSION && message.contains("type") && message["type"].is_string() && message["type"].get<std::string_view>() == expected_type;
    }

    /**
     * @brief Read an unsigned member no larger than a maximum.
     * @param message Parsed message.
     * @param name Member name.
     * @param maximum Largest accepted value.
     * @param output Receives the value.
     * @return True when the member is valid.
     */
    bool read_bounded_size(const json &message, const char *name, std::size_t maximum, std::size_t &output) {
      if (!message.contains(name) || !message[name].is_number_unsigned()) {
        return false;
      }
      const auto value = message[name].get<std::uint64_t>();
      if (value > maximum) {
        return false;
      }
      output = static_cast<std::size_t>(value);
      return true;
    }

    /**
     * @brief Read a finite number.
     * @param message Parsed message.
     * @param name Member name.
     * @param output Receives the value.
     * @return True when the member is valid.
     */
    bool read_finite_number(const json &message, const char *name, double &output) {
      if (!message.contains(name) || !message[name].is_number()) {
        return false;
      }
      output = message[name].get<double>();
      return std::isfinite(output);
    }

    /**
     * @brief Read the TV controller ID.
     * @param message Parsed message.
     * @param output Receives the ID.
     * @return True when the member is valid.
     */
    bool read_controller_id(const json &message, std::uint32_t &output) {
      if (!message.contains("controllerId") || !message["controllerId"].is_number_unsigned()) {
        return false;
      }
      const auto value = message["controllerId"].get<std::uint64_t>();
      if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
      }
      output = static_cast<std::uint32_t>(value);
      return true;
    }

    /**
     * @brief Read an optional boolean, false when absent.
     * @param message Parsed message.
     * @param name Member name.
     * @param output Receives the value.
     * @return True when the member is absent or valid.
     */
    bool read_optional_boolean(const json &message, const char *name, bool &output) {
      output = false;
      if (!message.contains(name)) {
        return true;
      }
      if (!message[name].is_boolean()) {
        return false;
      }
      output = message[name].get<bool>();
      return true;
    }

    /**
     * @brief Convert a Moonlight motor speed to the Gamepad API range.
     * @param value Motor speed, 0-65535.
     * @return Magnitude, 0-1.
     */
    double rumble_magnitude(std::uint32_t value) {
      return static_cast<double>(std::min<std::uint32_t>(value, 65535U)) / 65535.0;
    }

    /**
     * @brief Fill in a Moonlight input header and copy the packet out.
     * @param packet Packet whose header is completed.
     * @param magic Packet type.
     * @return The serialized packet.
     */
    template<class T>
    std::vector<std::uint8_t> serialize(T &packet, std::uint32_t magic) {
      packet.header.size = util::endian::big<std::uint32_t>(sizeof(T) - sizeof(std::uint32_t));
      packet.header.magic = util::endian::little<std::uint32_t>(magic);
      std::vector<std::uint8_t> bytes(sizeof(T));
      std::memcpy(bytes.data(), &packet, sizeof(T));
      return bytes;
    }
  }  // namespace

  input_api_t make_packet_input_api(std::function<void(std::vector<std::uint8_t> &&)> sink) {
    input_api_t api;
    api.controller_arrival = [sink](std::uint8_t slot, std::uint16_t, std::uint8_t type, std::uint32_t supported_buttons, std::uint16_t capabilities) {
      SS_CONTROLLER_ARRIVAL_PACKET packet {};
      packet.controllerNumber = slot;
      packet.type = type;
      packet.capabilities = util::endian::little(capabilities);
      packet.supportedButtonFlags = util::endian::little(supported_buttons);
      sink(serialize(packet, SS_CONTROLLER_ARRIVAL_MAGIC));
    };
    api.controller_state = [sink](std::uint8_t slot, std::uint16_t active_mask, const controller_state_t &state) {
      NV_MULTI_CONTROLLER_PACKET packet {};
      packet.headerB = util::endian::little<std::int16_t>(MC_HEADER_B);
      packet.controllerNumber = util::endian::little<std::int16_t>(slot);
      packet.activeGamepadMask = util::endian::little(static_cast<std::int16_t>(active_mask));
      packet.midB = util::endian::little<std::int16_t>(MC_MID_B);
      packet.buttonFlags = util::endian::little(static_cast<std::int16_t>(state.button_flags));
      packet.leftTrigger = state.left_trigger;
      packet.rightTrigger = state.right_trigger;
      packet.leftStickX = util::endian::little(state.left_stick_x);
      packet.leftStickY = util::endian::little(state.left_stick_y);
      packet.rightStickX = util::endian::little(state.right_stick_x);
      packet.rightStickY = util::endian::little(state.right_stick_y);
      packet.tailA = util::endian::little<std::int16_t>(MC_TAIL_A);
      packet.buttonFlags2 = util::endian::little(static_cast<std::int16_t>(state.button_flags >> 16));
      packet.tailB = util::endian::little<std::int16_t>(MC_TAIL_B);
      sink(serialize(packet, MULTI_CONTROLLER_MAGIC_GEN5));
    };
    api.mouse_move = [sink](std::int16_t delta_x, std::int16_t delta_y) {
      NV_REL_MOUSE_MOVE_PACKET packet {};
      packet.deltaX = util::endian::big(delta_x);
      packet.deltaY = util::endian::big(delta_y);
      sink(serialize(packet, MOUSE_MOVE_REL_MAGIC_GEN5));
    };
    api.mouse_button = [sink](bool pressed, std::uint8_t button) {
      NV_MOUSE_BUTTON_PACKET packet {};
      packet.button = button;
      sink(serialize(packet, pressed ? MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5));
    };
    api.scroll = [sink](std::int8_t clicks) {
      NV_SCROLL_PACKET packet {};
      packet.scrollAmt1 = util::endian::big(static_cast<std::int16_t>(clicks * WHEEL_DELTA_UNITS));
      packet.scrollAmt2 = packet.scrollAmt1;
      sink(serialize(packet, SCROLL_MAGIC_GEN5));
    };
    api.horizontal_scroll = [sink](std::int8_t clicks) {
      SS_HSCROLL_PACKET packet {};
      packet.scrollAmount = util::endian::big(static_cast<std::int16_t>(clicks * WHEEL_DELTA_UNITS));
      sink(serialize(packet, SS_HSCROLL_MAGIC));
    };
    return api;
  }

  input_bridge_t::input_bridge_t(input_api_t api, control_sender_t control_sender):
      _api(std::move(api)),
      _control_sender(std::move(control_sender)) {
    if (!_api.controller_arrival || !_api.controller_state || !_api.mouse_move || !_api.mouse_button || !_api.scroll || !_api.horizontal_scroll) {
      throw std::invalid_argument("Every input callback is required");
    }
    _worker = std::jthread([this](std::stop_token stop_token) {
      worker_loop(stop_token);
    });
  }

  input_bridge_t::~input_bridge_t() {
    shutdown();
  }

  bool input_bridge_t::handle_control_message(std::string_view text) {
    try {
      const json message = json::parse(text);
      std::uint32_t client_controller_id = 0;
      if (has_envelope(message, "gamepad-connected")) {
        if (!read_controller_id(message, client_controller_id) || !message.contains("id") || !message["id"].is_string() || !message.contains("mapping") || !message["mapping"].is_string()) {
          return false;
        }
        const auto id = message["id"].get<std::string>();
        const auto mapping = message["mapping"].get<std::string>();
        std::size_t button_count = 0;
        std::size_t axis_count = 0;
        bool dual_rumble = false;
        bool trigger_rumble = false;
        if (id.size() > 512 || mapping.size() > 128 || !read_bounded_size(message, "buttons", 64, button_count) || !read_bounded_size(message, "axes", 16, axis_count) || !read_optional_boolean(message, "dualRumble", dual_rumble) || !read_optional_boolean(message, "triggerRumble", trigger_rumble)) {
          return false;
        }

        std::lock_guard lock(_mutex);
        if (_shutdown) {
          return false;
        }
        remove_controller_locked(client_controller_id);
        const auto slot = allocate_slot_locked();
        if (!slot) {
          BOOST_LOG(warning) << "WebRTC: gamepad rejected, all 16 controller slots are occupied"sv;
          return false;
        }

        controller_t controller;
        controller.client_controller_id = client_controller_id;
        controller.slot = *slot;
        controller.id = id;
        controller.button_count = button_count;
        controller.type = detect_controller_type(id);
        if (button_count > 7) {
          controller.capabilities |= LI_CCAP_ANALOG_TRIGGERS;
        }
        if (dual_rumble) {
          controller.capabilities |= LI_CCAP_RUMBLE;
        }
        if (trigger_rumble) {
          controller.capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        }

        _slot_to_controller[*slot] = client_controller_id;
        _active_gamepad_mask |= static_cast<std::uint16_t>(1U << *slot);
        auto [entry, inserted] = _controllers.emplace(client_controller_id, std::move(controller));
        BOOST_LOG(info) << "WebRTC: gamepad connected, slot "sv << (int) *slot << ": "sv << id;
        announce_controller_locked(entry->second);
        return inserted;
      }

      if (has_envelope(message, "gamepad-disconnected")) {
        if (!read_controller_id(message, client_controller_id)) {
          return false;
        }
        std::lock_guard lock(_mutex);
        if (_shutdown) {
          return false;
        }
        const bool existed = _controllers.contains(client_controller_id);
        remove_controller_locked(client_controller_id);
        return existed;
      }
    } catch (const json::exception &) {
      return false;
    }
    return false;
  }

  bool input_bridge_t::handle_gamepad_message(std::string_view text, clock::time_point received_at) {
    std::uint32_t client_controller_id = 0;
    std::uint64_t sequence = 0;
    std::uint32_t buttons = 0;
    double timestamp = 0.0;
    double left_trigger = 0.0;
    double right_trigger = 0.0;
    double left_stick_x = 0.0;
    double left_stick_y = 0.0;
    double right_stick_x = 0.0;
    double right_stick_y = 0.0;

    try {
      const json message = json::parse(text);
      if (!has_envelope(message, "gamepad-state") || !read_controller_id(message, client_controller_id) || !message.contains("seq") || !message["seq"].is_number_unsigned() || !message.contains("buttons") || !message["buttons"].is_number_unsigned()) {
        return false;
      }
      sequence = message["seq"].get<std::uint64_t>();
      const auto raw_buttons = message["buttons"].get<std::uint64_t>();
      if (raw_buttons > ALL_PROTOCOL_BUTTONS || !read_finite_number(message, "timestampMs", timestamp) || timestamp < 0.0 || !read_finite_number(message, "leftTrigger", left_trigger) || !read_finite_number(message, "rightTrigger", right_trigger) || !read_finite_number(message, "leftStickX", left_stick_x) || !read_finite_number(message, "leftStickY", left_stick_y) || !read_finite_number(message, "rightStickX", right_stick_x) || !read_finite_number(message, "rightStickY", right_stick_y)) {
        return false;
      }
      buttons = static_cast<std::uint32_t>(raw_buttons);
    } catch (const json::exception &) {
      return false;
    }

    controller_state_t state;
    state.button_flags = map_standard_buttons(buttons);
    state.left_trigger = map_trigger(left_trigger);
    state.right_trigger = map_trigger(right_trigger);
    state.left_stick_x = map_stick(left_stick_x, false);
    state.left_stick_y = map_stick(left_stick_y, true);
    state.right_stick_x = map_stick(right_stick_x, false);
    state.right_stick_y = map_stick(right_stick_y, true);

    std::lock_guard lock(_mutex);
    const auto entry = _controllers.find(client_controller_id);
    if (_shutdown || entry == _controllers.end()) {
      return false;
    }
    auto &controller = entry->second;
    // Snapshots travel on an unordered, unreliable channel; an older one must not undo a newer one.
    if (controller.last_sequence && sequence <= *controller.last_sequence) {
      ++controller.stale_states;
      return false;
    }
    if (controller.last_sequence && sequence > *controller.last_sequence + 1) {
      controller.sequence_gaps += sequence - *controller.last_sequence - 1;
    }
    controller.last_sequence = sequence;
    update_diagnostics_locked(controller, received_at);
    controller.latest_state = state;

    const bool start_pressed = has_button(buttons, gamepad_button_e::start);
    const bool start_was_pressed = has_button(controller.previous_buttons, gamepad_button_e::start);
    if (start_pressed && !start_was_pressed) {
      controller.start_pressed_at = received_at;
    }

    bool mouse_mode_toggled = false;
    if (!start_pressed && start_was_pressed && controller.start_pressed_at && received_at - *controller.start_pressed_at > MOUSE_EMULATION_LONG_PRESS_TIME) {
      if (!controller.mouse_mode && _stream_active && controller.announced) {
        _api.controller_state(controller.slot, _active_gamepad_mask, state);
      }
      if (controller.mouse_mode) {
        release_mouse_buttons_locked(controller);
      }
      controller.mouse_mode = !controller.mouse_mode;
      controller.suppressed_mouse_buttons = controller.mouse_mode ? buttons & MOUSE_BUTTON_MASK : 0;
      queue_mouse_mode_status_locked(controller);
      BOOST_LOG(info) << "WebRTC: gamepad mouse mode "sv << (controller.mouse_mode ? "enabled"sv : "disabled"sv) << ", slot "sv << (int) controller.slot;
      mouse_mode_toggled = true;
    }
    if (!start_pressed) {
      controller.start_pressed_at.reset();
    }

    if (_stream_active && controller.announced) {
      if (controller.mouse_mode) {
        const auto effective = buttons & ~controller.suppressed_mouse_buttons;
        const auto previous_effective = controller.previous_buttons & ~controller.suppressed_mouse_buttons;
        if (!mouse_mode_toggled) {
          for (const auto button : MOUSE_BUTTONS) {
            const bool pressed = has_button(effective, button);
            if (pressed != has_button(previous_effective, button)) {
              _api.mouse_button(pressed, moonlight_mouse_button(button));
            }
          }
          const auto rising = [&](gamepad_button_e button) {
            return has_button(buttons, button) && !has_button(controller.previous_buttons, button);
          };
          if (rising(gamepad_button_e::dpad_up)) {
            _api.scroll(1);
          }
          if (rising(gamepad_button_e::dpad_down)) {
            _api.scroll(-1);
          }
          if (rising(gamepad_button_e::dpad_right)) {
            _api.horizontal_scroll(1);
          }
          if (rising(gamepad_button_e::dpad_left)) {
            _api.horizontal_scroll(-1);
          }
        }
        controller.suppressed_mouse_buttons &= buttons;
        controller.simulated_mouse_buttons = effective & MOUSE_BUTTON_MASK;
      } else {
        _api.controller_state(controller.slot, _active_gamepad_mask, state);
      }
    }
    controller.previous_buttons = buttons;
    return true;
  }

  void input_bridge_t::handle_rumble(std::uint16_t controller_number, std::uint16_t low_frequency_motor, std::uint16_t high_frequency_motor) {
    std::lock_guard lock(_mutex);
    if (_shutdown || controller_number >= _slot_to_controller.size() || !_slot_to_controller[controller_number]) {
      return;
    }
    auto &controller = _controllers.at(*_slot_to_controller[controller_number]);
    if ((controller.capabilities & LI_CCAP_RUMBLE) == 0) {
      return;
    }
    controller.strong_magnitude = rumble_magnitude(low_frequency_motor);
    controller.weak_magnitude = rumble_magnitude(high_frequency_motor);
    queue_rumble_state_locked(controller);
  }

  void input_bridge_t::handle_trigger_rumble(std::uint16_t controller_number, std::uint16_t left_trigger_motor, std::uint16_t right_trigger_motor) {
    std::lock_guard lock(_mutex);
    if (_shutdown || controller_number >= _slot_to_controller.size() || !_slot_to_controller[controller_number]) {
      return;
    }
    auto &controller = _controllers.at(*_slot_to_controller[controller_number]);
    if ((controller.capabilities & LI_CCAP_TRIGGER_RUMBLE) == 0) {
      return;
    }
    controller.left_trigger_magnitude = rumble_magnitude(left_trigger_motor);
    controller.right_trigger_magnitude = rumble_magnitude(right_trigger_motor);
    queue_rumble_state_locked(controller);
  }

  void input_bridge_t::set_stream_active(bool active) {
    std::lock_guard lock(_mutex);
    if (_shutdown || _stream_active == active) {
      return;
    }
    if (active) {
      _stream_active = true;
      for (auto &[client_controller_id, controller] : _controllers) {
        announce_controller_locked(controller);
      }
    } else {
      clear_controllers_locked();
      _stream_active = false;
    }
  }

  void input_bridge_t::on_transport_closed() {
    std::lock_guard lock(_mutex);
    if (!_shutdown) {
      clear_controllers_locked();
    }
  }

  void input_bridge_t::shutdown() {
    {
      std::lock_guard lock(_mutex);
      if (_shutdown) {
        return;
      }
      clear_controllers_locked();
      _stream_active = false;
      _shutdown = true;
    }
    _worker.request_stop();
    _worker_condition.notify_all();
    if (_worker.joinable() && _worker.get_id() != std::this_thread::get_id()) {
      _worker.join();
    }
  }

  int input_bridge_t::map_standard_buttons(std::uint32_t protocol_buttons) {
    int flags = 0;
    const auto map = [&](gamepad_button_e input, int output) {
      if (has_button(protocol_buttons, input)) {
        flags |= output;
      }
    };
    map(gamepad_button_e::a, A_FLAG);
    map(gamepad_button_e::b, B_FLAG);
    map(gamepad_button_e::x, X_FLAG);
    map(gamepad_button_e::y, Y_FLAG);
    map(gamepad_button_e::left_bumper, LB_FLAG);
    map(gamepad_button_e::right_bumper, RB_FLAG);
    map(gamepad_button_e::back, BACK_FLAG);
    map(gamepad_button_e::start, PLAY_FLAG);
    map(gamepad_button_e::left_stick, LS_CLK_FLAG);
    map(gamepad_button_e::right_stick, RS_CLK_FLAG);
    map(gamepad_button_e::dpad_up, UP_FLAG);
    map(gamepad_button_e::dpad_down, DOWN_FLAG);
    map(gamepad_button_e::dpad_left, LEFT_FLAG);
    map(gamepad_button_e::dpad_right, RIGHT_FLAG);
    map(gamepad_button_e::guide, SPECIAL_FLAG);
    return flags;
  }

  std::uint8_t input_bridge_t::map_trigger(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
  }

  std::int16_t input_bridge_t::map_stick(double value, bool invert) {
    double clamped = std::clamp(value, -1.0, 1.0);
    if (invert) {
      clamped = -clamped;
    }
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0));
  }

  std::uint32_t input_bridge_t::standard_supported_button_flags(std::size_t button_count) {
    std::uint32_t flags = 0;
    const auto include = [&](std::size_t index, std::uint32_t flag) {
      if (button_count > index) {
        flags |= flag;
      }
    };
    include(0, A_FLAG);
    include(1, B_FLAG);
    include(2, X_FLAG);
    include(3, Y_FLAG);
    include(4, LB_FLAG);
    include(5, RB_FLAG);
    include(8, BACK_FLAG);
    include(9, PLAY_FLAG);
    include(10, LS_CLK_FLAG);
    include(11, RS_CLK_FLAG);
    include(12, UP_FLAG);
    include(13, DOWN_FLAG);
    include(14, LEFT_FLAG);
    include(15, RIGHT_FLAG);
    include(16, SPECIAL_FLAG);
    return flags;
  }

  std::uint8_t input_bridge_t::detect_controller_type(std::string_view id) {
    std::string normalized(id);
    std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    const auto contains = [&normalized](std::initializer_list<std::string_view> needles) {
      return std::ranges::any_of(needles, [&normalized](std::string_view needle) {
        return normalized.find(needle) != std::string::npos;
      });
    };
    if (contains({"vendor: 045e", "vendor 045e", "xbox", "microsoft"})) {
      return LI_CTYPE_XBOX;
    }
    if (contains({"vendor: 054c", "vendor 054c", "dualshock", "dualsense", "playstation", "sony"})) {
      return LI_CTYPE_PS;
    }
    if (contains({"vendor: 057e", "vendor 057e", "nintendo", "joy-con", "switch pro"})) {
      return LI_CTYPE_NINTENDO;
    }
    return LI_CTYPE_UNKNOWN;
  }

  std::pair<std::int16_t, std::int16_t> input_bridge_t::mouse_delta(const controller_state_t &state) {
    const int left_y = -static_cast<int>(state.left_stick_y);
    const int right_y = -static_cast<int>(state.right_stick_y);
    const bool use_left = std::abs(static_cast<int>(state.left_stick_x)) + std::abs(left_y) > std::abs(static_cast<int>(state.right_stick_x)) + std::abs(right_y);
    const int raw_x = use_left ? state.left_stick_x : state.right_stick_x;
    const int raw_y = use_left ? left_y : right_y;
    const auto accelerate = [](int raw) {
      double delta = std::pow(raw / 32766.0 * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
      delta = std::abs(delta) > MOUSE_EMULATION_DEADZONE ? delta - MOUSE_EMULATION_DEADZONE : 0.0;
      return static_cast<std::int16_t>(std::clamp<long>(std::lround(delta), std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    };
    return {accelerate(raw_x), accelerate(raw_y)};
  }

  std::optional<std::uint8_t> input_bridge_t::allocate_slot_locked() const {
    for (std::size_t index = 0; index < _slot_to_controller.size(); ++index) {
      if (!_slot_to_controller[index]) {
        return static_cast<std::uint8_t>(index);
      }
    }
    return std::nullopt;
  }

  void input_bridge_t::announce_controller_locked(controller_t &controller) {
    if (!_stream_active || controller.announced) {
      return;
    }
    _api.controller_arrival(controller.slot, _active_gamepad_mask, controller.type, standard_supported_button_flags(controller.button_count), controller.capabilities);
    controller.announced = true;
    queue_control_message_locked(json {
      {"v", GAMEPAD_PROTOCOL_VERSION},
      {"type", "controller-status"},
      {"controllerId", controller.client_controller_id},
      {"controllerSlot", controller.slot},
      {"activeGamepadMask", _active_gamepad_mask},
      {"controllerType", controller.type},
      {"capabilities", controller.capabilities},
    }
                                   .dump());
  }

  void input_bridge_t::neutralize_controller_locked(controller_t &controller) {
    release_mouse_buttons_locked(controller);
    if (controller.mouse_mode) {
      controller.mouse_mode = false;
      queue_mouse_mode_status_locked(controller);
    }
    controller.strong_magnitude = 0.0;
    controller.weak_magnitude = 0.0;
    controller.left_trigger_magnitude = 0.0;
    controller.right_trigger_magnitude = 0.0;
    queue_rumble_state_locked(controller);

    const auto slot_bit = static_cast<std::uint16_t>(1U << controller.slot);
    if (_stream_active && controller.announced) {
      // Release everything first, then drop the slot from the mask so the host unplugs it.
      _api.controller_state(controller.slot, _active_gamepad_mask, controller_state_t {});
      _active_gamepad_mask &= static_cast<std::uint16_t>(~slot_bit);
      _api.controller_state(controller.slot, _active_gamepad_mask, controller_state_t {});
    } else {
      _active_gamepad_mask &= static_cast<std::uint16_t>(~slot_bit);
    }
    controller.announced = false;
  }

  void input_bridge_t::release_mouse_buttons_locked(controller_t &controller) {
    for (const auto button : MOUSE_BUTTONS) {
      if (has_button(controller.simulated_mouse_buttons, button)) {
        _api.mouse_button(false, moonlight_mouse_button(button));
      }
    }
    controller.simulated_mouse_buttons = 0;
    controller.suppressed_mouse_buttons = 0;
  }

  void input_bridge_t::remove_controller_locked(std::uint32_t client_controller_id) {
    const auto entry = _controllers.find(client_controller_id);
    if (entry == _controllers.end()) {
      return;
    }
    const auto slot = entry->second.slot;
    neutralize_controller_locked(entry->second);
    _slot_to_controller[slot].reset();
    _controllers.erase(entry);
    BOOST_LOG(info) << "WebRTC: gamepad disconnected, slot "sv << (int) slot;
  }

  void input_bridge_t::clear_controllers_locked() {
    while (!_controllers.empty()) {
      remove_controller_locked(_controllers.begin()->first);
    }
    _active_gamepad_mask = 0;
  }

  void input_bridge_t::update_diagnostics_locked(controller_t &controller, clock::time_point received_at) {
    if (!controller.rate_window_start) {
      controller.rate_window_start = received_at;
    }
    ++controller.states_in_rate_window;
    const double elapsed = std::chrono::duration<double>(received_at - *controller.rate_window_start).count();
    if (elapsed < 1.0) {
      return;
    }
    queue_control_message_locked(json {
      {"v", GAMEPAD_PROTOCOL_VERSION},
      {"type", "input-diagnostics"},
      {"controllerId", controller.client_controller_id},
      {"controllerSlot", controller.slot},
      {"messagesPerSecond", static_cast<double>(controller.states_in_rate_window) / elapsed},
      {"lastSequence", controller.last_sequence.value_or(0)},
      {"sequenceGaps", controller.sequence_gaps},
      {"staleStates", controller.stale_states},
    }
                                   .dump());
    controller.rate_window_start = received_at;
    controller.states_in_rate_window = 0;
  }

  void input_bridge_t::queue_control_message_locked(std::string message) {
    if (!_control_sender) {
      return;
    }
    _pending_control_messages.push_back(std::move(message));
    _worker_condition.notify_all();
  }

  void input_bridge_t::queue_mouse_mode_status_locked(const controller_t &controller) {
    queue_control_message_locked(json {
      {"v", GAMEPAD_PROTOCOL_VERSION},
      {"type", "mouse-mode"},
      {"controllerId", controller.client_controller_id},
      {"controllerSlot", controller.slot},
      {"active", controller.mouse_mode},
    }
                                   .dump());
  }

  void input_bridge_t::queue_rumble_state_locked(const controller_t &controller) {
    queue_control_message_locked(json {
      {"v", GAMEPAD_PROTOCOL_VERSION},
      {"type", "rumble"},
      {"controllerId", controller.client_controller_id},
      {"controllerSlot", controller.slot},
      {"strongMagnitude", controller.strong_magnitude},
      {"weakMagnitude", controller.weak_magnitude},
      {"leftTrigger", controller.left_trigger_magnitude},
      {"rightTrigger", controller.right_trigger_magnitude},
    }
                                   .dump());
  }

  void input_bridge_t::worker_loop(std::stop_token stop_token) {
    auto next_mouse_tick = clock::now() + MOUSE_EMULATION_POLLING_INTERVAL;
    while (!stop_token.stop_requested()) {
      std::vector<std::string> messages;
      std::vector<std::pair<std::int16_t, std::int16_t>> movements;
      {
        std::unique_lock lock(_mutex);
        _worker_condition.wait_until(lock, next_mouse_tick, [&] {
          return _shutdown || !_pending_control_messages.empty() || stop_token.stop_requested();
        });
        if (_shutdown || stop_token.stop_requested()) {
          break;
        }
        messages.swap(_pending_control_messages);
        const auto now = clock::now();
        if (now >= next_mouse_tick) {
          for (const auto &[client_controller_id, controller] : _controllers) {
            if (_stream_active && controller.announced && controller.mouse_mode) {
              const auto delta = mouse_delta(controller.latest_state);
              if (delta.first != 0 || delta.second != 0) {
                movements.push_back(delta);
              }
            }
          }
          next_mouse_tick = now + MOUSE_EMULATION_POLLING_INTERVAL;
        }
      }
      // Sent outside the lock: the DataChannel may block, and rumble arrives on other threads.
      for (const auto &message : messages) {
        _control_sender(message);
      }
      for (const auto &[delta_x, delta_y] : movements) {
        _api.mouse_move(delta_x, delta_y);
      }
    }
  }
}  // namespace webrtc_stream
