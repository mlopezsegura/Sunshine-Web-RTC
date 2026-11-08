/**
 * @file src/platform/windows/session_input.h
 * @brief Session-local keyboard and mouse injection for non-console sessions.
 *
 * libvirtualhid prefers a driver-backed HID mouse and keyboard when the
 * Virtual HID Driver is installed and licensed. Windows routes physical (HID)
 * input to the session attached to the console, so when Sunshine runs inside
 * an RDP session the host console receives the client's mouse and keyboard.
 * SendInput() injects into the caller's own session, so these helpers bypass
 * libvirtualhid for mouse and keyboard whenever Sunshine is not running in
 * the active console session.
 */
#pragma once

// standard includes
#include <cstdint>

// local includes
#include "src/platform/common.h"

namespace platf::session_input {
  /**
   * @brief Whether mouse and keyboard must bypass driver-backed HID devices.
   *
   * @return true when the current process is not in the active console session.
   */
  bool should_use();

  void move_mouse(int delta_x, int delta_y);
  void abs_mouse(const touch_port_t &touch_port, float x, float y);
  void button_mouse(int button, bool release);
  void scroll(int high_res_distance);
  void hscroll(int high_res_distance);
  void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t flags, bool extended);
  void unicode(const char *utf8, int size);
}  // namespace platf::session_input
