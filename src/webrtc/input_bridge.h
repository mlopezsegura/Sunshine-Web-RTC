/**
 * @file src/webrtc/input_bridge.h
 * @brief Declarations for translating TV gamepad messages into Sunshine input.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webrtc_stream {
  constexpr std::uint32_t GAMEPAD_PROTOCOL_VERSION = 2;  ///< Version of the gamepad DataChannel protocol.
  constexpr std::size_t MAXIMUM_GAMEPADS = 16;  ///< Controller slots in Moonlight's active gamepad mask.
  constexpr auto MOUSE_EMULATION_LONG_PRESS_TIME = std::chrono::milliseconds(750);  ///< Start hold that toggles mouse mode.
  constexpr auto MOUSE_EMULATION_POLLING_INTERVAL = std::chrono::milliseconds(50);  ///< Mouse emulation update period.
  constexpr double MOUSE_EMULATION_MOTION_MULTIPLIER = 4.0;  ///< Stick-to-pointer speed factor.
  constexpr double MOUSE_EMULATION_DEADZONE = 2.0;  ///< Pointer deadzone in pixels per update.

  /**
   * @brief Standard Gamepad API buttons as bits of the TV's button mask.
   */
  enum class gamepad_button_e : std::uint32_t {
    a = 1U << 0,  ///< A / Cross.
    b = 1U << 1,  ///< B / Circle.
    x = 1U << 2,  ///< X / Square.
    y = 1U << 3,  ///< Y / Triangle.
    left_bumper = 1U << 4,  ///< Left bumper.
    right_bumper = 1U << 5,  ///< Right bumper.
    back = 1U << 6,  ///< Back / Select.
    start = 1U << 7,  ///< Start.
    left_stick = 1U << 8,  ///< Left stick click.
    right_stick = 1U << 9,  ///< Right stick click.
    dpad_up = 1U << 10,  ///< D-pad up.
    dpad_down = 1U << 11,  ///< D-pad down.
    dpad_left = 1U << 12,  ///< D-pad left.
    dpad_right = 1U << 13,  ///< D-pad right.
    guide = 1U << 14,  ///< Guide / Home.
  };

  /**
   * @brief Controller state in Moonlight's representation.
   */
  struct controller_state_t {
    int button_flags = 0;  ///< Moonlight button flags.
    std::uint8_t left_trigger = 0;  ///< Left trigger, 0-255.
    std::uint8_t right_trigger = 0;  ///< Right trigger, 0-255.
    std::int16_t left_stick_x = 0;  ///< Left stick X.
    std::int16_t left_stick_y = 0;  ///< Left stick Y, up positive.
    std::int16_t right_stick_x = 0;  ///< Right stick X.
    std::int16_t right_stick_y = 0;  ///< Right stick Y, up positive.
  };

  /**
   * @brief Moonlight input events the bridge emits.
   */
  struct input_api_t {
    std::function<void(std::uint8_t, std::uint16_t, std::uint8_t, std::uint32_t, std::uint16_t)> controller_arrival;  ///< Slot, mask, type, supported buttons, capabilities.
    std::function<void(std::uint8_t, std::uint16_t, const controller_state_t &)> controller_state;  ///< Slot, mask, state.
    std::function<void(std::int16_t, std::int16_t)> mouse_move;  ///< Relative pointer motion.
    std::function<void(bool, std::uint8_t)> mouse_button;  ///< Press flag and Moonlight button.
    std::function<void(std::int8_t)> scroll;  ///< Vertical wheel clicks.
    std::function<void(std::int8_t)> horizontal_scroll;  ///< Horizontal wheel clicks.
  };

  /**
   * @brief Build an input API that feeds Moonlight input packets to a sink.
   * @param sink Receives each serialized Moonlight input packet.
   * @return The input API.
   */
  input_api_t make_packet_input_api(std::function<void(std::vector<std::uint8_t> &&)> sink);

  /**
   * @brief Translates the TV's gamepad DataChannel messages into Moonlight controller input.
   *
   * Controllers announced by the TV get a Moonlight slot each. Holding Start toggles mouse
   * emulation. Rumble from the host is relayed to the TV through the control channel.
   */
  class input_bridge_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock for timing and diagnostics.
    using control_sender_t = std::function<void(const std::string &)>;  ///< Sends a control message to the TV.

    /**
     * @brief Create a bridge.
     * @param api Input events to emit.
     * @param control_sender Sends control messages to the TV.
     */
    input_bridge_t(input_api_t api, control_sender_t control_sender);

    /**
     * @brief Stop the bridge.
     */
    ~input_bridge_t();

    input_bridge_t(const input_bridge_t &) = delete;
    input_bridge_t &operator=(const input_bridge_t &) = delete;

    /**
     * @brief Handle a control-channel message (controller connected or disconnected).
     * @param text JSON message.
     * @return True when the message was valid and applied.
     */
    bool handle_control_message(std::string_view text);

    /**
     * @brief Handle a gamepad snapshot.
     * @param text JSON message.
     * @param received_at Arrival time.
     * @return True when the snapshot was valid, current and applied.
     */
    bool handle_gamepad_message(std::string_view text, clock::time_point received_at = clock::now());

    /**
     * @brief Start or stop forwarding input to the host.
     * @param active Whether the host stream is running.
     */
    void set_stream_active(bool active);

    /**
     * @brief Neutralize and forget every controller after the TV's channels closed.
     */
    void on_transport_closed();

    /**
     * @brief Neutralize every controller and stop the worker thread.
     */
    void shutdown();

    /**
     * @brief Relay rumble for a Moonlight controller slot to the TV.
     * @param controller_number Moonlight slot.
     * @param low_frequency_motor Strong motor, 0-65535.
     * @param high_frequency_motor Weak motor, 0-65535.
     */
    void handle_rumble(std::uint16_t controller_number, std::uint16_t low_frequency_motor, std::uint16_t high_frequency_motor);

    /**
     * @brief Relay trigger rumble for a Moonlight controller slot to the TV.
     * @param controller_number Moonlight slot.
     * @param left_trigger_motor Left trigger motor, 0-65535.
     * @param right_trigger_motor Right trigger motor, 0-65535.
     */
    void handle_trigger_rumble(std::uint16_t controller_number, std::uint16_t left_trigger_motor, std::uint16_t right_trigger_motor);

    /**
     * @brief Map the TV's button mask to Moonlight button flags.
     * @param protocol_buttons TV button mask.
     * @return Moonlight button flags.
     */
    static int map_standard_buttons(std::uint32_t protocol_buttons);

    /**
     * @brief Map a trigger value to Moonlight's range.
     * @param value Trigger value, 0-1.
     * @return Trigger value, 0-255.
     */
    static std::uint8_t map_trigger(double value);

    /**
     * @brief Map a stick axis to Moonlight's range.
     * @param value Axis value, -1 to 1.
     * @param invert Whether to invert the axis (Gamepad API Y is down positive).
     * @return Axis value, -32767 to 32767.
     */
    static std::int16_t map_stick(double value, bool invert);

    /**
     * @brief Moonlight buttons a controller supports, from its Gamepad API button count.
     * @param button_count Number of buttons reported by the TV.
     * @return Moonlight button flags.
     */
    static std::uint32_t standard_supported_button_flags(std::size_t button_count);

    /**
     * @brief Guess the Moonlight controller type from a Gamepad API ID.
     * @param id Gamepad API ID string.
     * @return Moonlight controller type.
     */
    static std::uint8_t detect_controller_type(std::string_view id);

    /**
     * @brief Pointer motion for one mouse-emulation update.
     * @param state Controller state.
     * @return Relative X and Y motion.
     */
    static std::pair<std::int16_t, std::int16_t> mouse_delta(const controller_state_t &state);

  private:
    /**
     * @brief State of one TV controller.
     */
    struct controller_t {
      std::uint32_t client_controller_id = 0;  ///< ID assigned by the TV.
      std::uint8_t slot = 0;  ///< Moonlight slot.
      std::string id;  ///< Gamepad API ID.
      std::size_t button_count = 0;  ///< Gamepad API button count.
      std::uint8_t type = 0;  ///< Moonlight controller type.
      std::uint16_t capabilities = 0;  ///< Moonlight controller capabilities.
      bool announced = false;  ///< Whether the host knows the controller.
      controller_state_t latest_state;  ///< Latest state.
      std::uint32_t previous_buttons = 0;  ///< Button mask of the previous snapshot.
      std::optional<std::uint64_t> last_sequence;  ///< Sequence of the latest accepted snapshot.
      std::uint64_t sequence_gaps = 0;  ///< Snapshots lost so far.
      std::uint64_t stale_states = 0;  ///< Out-of-order snapshots dropped so far.
      std::optional<clock::time_point> start_pressed_at;  ///< When Start was pressed.
      bool mouse_mode = false;  ///< Whether the controller drives the pointer.
      std::uint32_t suppressed_mouse_buttons = 0;  ///< Buttons held while toggling mouse mode.
      std::uint32_t simulated_mouse_buttons = 0;  ///< Mouse buttons currently pressed.
      double strong_magnitude = 0.0;  ///< Strong rumble motor.
      double weak_magnitude = 0.0;  ///< Weak rumble motor.
      double left_trigger_magnitude = 0.0;  ///< Left trigger motor.
      double right_trigger_magnitude = 0.0;  ///< Right trigger motor.
      std::optional<clock::time_point> rate_window_start;  ///< Start of the rate measurement window.
      std::uint64_t states_in_rate_window = 0;  ///< Snapshots in the rate window.
    };

    /**
     * @brief Find a free controller slot.
     * @return The slot, or nothing when all are taken.
     */
    std::optional<std::uint8_t> allocate_slot_locked() const;
    /**
     * @brief Announce a controller to the host once the stream is active.
     * @param controller Controller.
     */
    void announce_controller_locked(controller_t &controller);
    /**
     * @brief Release everything a controller holds and unplug it from the host.
     * @param controller Controller.
     */
    void neutralize_controller_locked(controller_t &controller);
    /**
     * @brief Release the mouse buttons a controller holds in mouse mode.
     * @param controller Controller.
     */
    void release_mouse_buttons_locked(controller_t &controller);
    /**
     * @brief Neutralize and forget a controller.
     * @param client_controller_id TV controller ID.
     */
    void remove_controller_locked(std::uint32_t client_controller_id);
    /**
     * @brief Neutralize and forget every controller.
     */
    void clear_controllers_locked();
    /**
     * @brief Count a snapshot and report the rate to the TV once a second.
     * @param controller Controller.
     * @param received_at Arrival time.
     */
    void update_diagnostics_locked(controller_t &controller, clock::time_point received_at);
    /**
     * @brief Queue a message for the control channel.
     * @param message JSON message.
     */
    void queue_control_message_locked(std::string message);
    /**
     * @brief Tell the TV whether a controller is in mouse mode.
     * @param controller Controller.
     */
    void queue_mouse_mode_status_locked(const controller_t &controller);
    /**
     * @brief Send a controller's rumble state to the TV.
     * @param controller Controller.
     */
    void queue_rumble_state_locked(const controller_t &controller);
    /**
     * @brief Send queued control messages and emulated mouse motion.
     * @param stop_token Stops the loop.
     */
    void worker_loop(std::stop_token stop_token);

    input_api_t _api;  ///< Input events to emit.
    control_sender_t _control_sender;  ///< Sends control messages to the TV.
    mutable std::mutex _mutex;  ///< Protects the state below.
    std::condition_variable _worker_condition;  ///< Wakes the worker.
    std::jthread _worker;  ///< Sends control messages and mouse motion.
    std::vector<std::string> _pending_control_messages;  ///< Control messages not sent yet.
    std::unordered_map<std::uint32_t, controller_t> _controllers;  ///< Controllers by TV ID.
    std::array<std::optional<std::uint32_t>, MAXIMUM_GAMEPADS> _slot_to_controller;  ///< TV ID by slot.
    std::uint16_t _active_gamepad_mask = 0;  ///< Moonlight active gamepad mask.
    bool _stream_active = false;  ///< Whether input reaches the host.
    bool _shutdown = false;  ///< Whether the bridge stopped.
  };
}  // namespace webrtc_stream
