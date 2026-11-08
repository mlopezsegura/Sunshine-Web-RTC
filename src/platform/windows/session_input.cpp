/**
 * @file src/platform/windows/session_input.cpp
 * @brief Session-local keyboard and mouse injection for non-console sessions.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

// platform includes
#include <Windows.h>

// local includes
#include "misc.h"
#include "session_input.h"
#include "src/config.h"
#include "src/logging.h"
#include "third-party/libvirtualhid/src/platform/windows/keylayout.hpp"

using namespace std::literals;

namespace platf::session_input {
  namespace {
    /**
     * @brief Input desktop last bound to this thread, used to retry SendInput after a desktop switch.
     */
    thread_local HDESK last_input_desktop = nullptr;

    /**
     * @brief Call SendInput() and retry once on the new input desktop (UAC, lock screen).
     *
     * @param inputs Events to inject.
     */
    void send(std::span<INPUT> inputs) {
      const auto count = static_cast<UINT>(inputs.size());
      if (SendInput(count, inputs.data(), sizeof(INPUT)) == count) {
        return;
      }

      if (const auto desktop = syncThreadDesktop(); desktop != last_input_desktop) {
        last_input_desktop = desktop;
        if (SendInput(count, inputs.data(), sizeof(INPUT)) == count) {
          return;
        }
      }

      BOOST_LOG(error) << "Couldn't send session-local input: "sv << GetLastError();
    }

    void send(INPUT input) {
      std::array inputs {input};
      send(std::span<INPUT> {inputs});
    }

    INPUT mouse_input(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
      INPUT input {};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = flags;
      input.mi.dx = dx;
      input.mi.dy = dy;
      input.mi.mouseData = data;
      return input;
    }

    /**
     * @brief Scale a coordinate inside [0, dimension] to the 0..65535 range used by MOUSEEVENTF_ABSOLUTE.
     */
    LONG scale_absolute_axis(float value, int dimension) {
      if (dimension <= 1) {
        return 0;
      }

      const auto clamped = std::clamp(value, 0.0F, static_cast<float>(dimension));
      return static_cast<LONG>(std::lround(clamped * static_cast<float>(std::numeric_limits<std::uint16_t>::max()) / static_cast<float>(dimension)));
    }

    /**
     * @brief Map a viewport-relative coordinate into the virtual desktop (same math as libvirtualhid's Win32 mouse).
     */
    LONG scale_to_desktop(float value, int viewport_offset, int viewport_dimension, int desktop_offset, int desktop_dimension) {
      if (viewport_dimension <= 0 || desktop_dimension <= 0) {
        return 0;
      }

      const auto clamped = std::clamp(value, 0.0F, static_cast<float>(viewport_dimension));
      const auto span = static_cast<float>(std::max(viewport_dimension - 1, 0));
      const auto target = static_cast<float>(viewport_offset - desktop_offset) + clamped * span / static_cast<float>(viewport_dimension);
      return scale_absolute_axis(target, desktop_dimension);
    }

    bool extended_key(std::uint16_t key_code) {
      switch (key_code) {
        case VK_LWIN:
        case VK_RWIN:
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_DIVIDE:
        case VK_APPS:
          return true;
        default:
          return false;
      }
    }
  }  // namespace

  bool should_use() {
    static const DWORD own_session = [] {
      DWORD session = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      return session;
    }();

    // The console can move between sessions while streaming (e.g. someone logs
    // in locally), so compare on every event. 0xFFFFFFFF means "no console
    // session right now": SendInput is still the right choice then.
    const bool bypass = WTSGetActiveConsoleSessionId() != own_session;

    static std::atomic<int> logged {-1};
    if (const int state = bypass ? 1 : 0; logged.exchange(state) != state) {
      if (bypass) {
        BOOST_LOG(info) << "Sunshine runs outside the console session ("sv << own_session
                        << "): mouse and keyboard use SendInput instead of Virtual HID devices"sv;
      } else {
        BOOST_LOG(info) << "Sunshine runs in the console session: mouse and keyboard use libvirtualhid"sv;
      }
    }
    return bypass;
  }

  void move_mouse(int delta_x, int delta_y) {
    send(mouse_input(MOUSEEVENTF_MOVE, delta_x, delta_y));
  }

  void abs_mouse(const touch_port_t &touch_port, float x, float y) {
    // Same desktop/viewport split as virtualhid::pointer_viewports(): the
    // desktop is the whole virtual screen, the viewport the streamed display.
    const bool has_logical = touch_port.logical_width > 0 && touch_port.logical_height > 0;
    const int viewport_x = has_logical ? touch_port.offset_x : touch_port.env_offset_x;
    const int viewport_y = has_logical ? touch_port.offset_y : touch_port.env_offset_y;
    const int viewport_w = has_logical ? touch_port.logical_width : touch_port.width;
    const int viewport_h = has_logical ? touch_port.logical_height : touch_port.height;

    const auto dx = scale_to_desktop(x - static_cast<float>(viewport_x), viewport_x, viewport_w, touch_port.env_offset_x, touch_port.width);
    const auto dy = scale_to_desktop(y - static_cast<float>(viewport_y), viewport_y, viewport_h, touch_port.env_offset_y, touch_port.height);
    send(mouse_input(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, dx, dy));
  }

  void button_mouse(int button, bool release) {
    switch (button) {
      case BUTTON_LEFT:
        send(mouse_input(release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN));
        return;
      case BUTTON_MIDDLE:
        send(mouse_input(release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN));
        return;
      case BUTTON_RIGHT:
        send(mouse_input(release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN));
        return;
      case BUTTON_X1:
        send(mouse_input(release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN, 0, 0, XBUTTON1));
        return;
      case BUTTON_X2:
        send(mouse_input(release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN, 0, 0, XBUTTON2));
        return;
      default:
        BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
        return;
    }
  }

  void scroll(int high_res_distance) {
    send(mouse_input(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(high_res_distance)));
  }

  void hscroll(int high_res_distance) {
    send(mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(high_res_distance)));
  }

  void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t flags, bool extended) {
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    auto &ki = input.ki;
    ki.wVk = modcode;

    // Same rules as libvirtualhid's Win32 keyboard: normalized VK codes map
    // through the US English layout; otherwise only map when asked to.
    if (!(flags & SS_KBE_FLAG_NON_NORMALIZED)) {
      ki.wScan = lvh::detail::windows_us_english_scan_code(modcode);
    } else if (config::input.always_send_scancodes && modcode != VK_LWIN && modcode != VK_RWIN && modcode != VK_PAUSE) {
      ki.wScan = static_cast<WORD>(MapVirtualKeyW(modcode, MAPVK_VK_TO_VSC));
    }

    if (ki.wScan != 0) {
      ki.wVk = 0;
      ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    if (extended || extended_key(modcode)) {
      ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (release) {
      ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    send(input);
  }

  void unicode(const char *utf8, int size) {
    if (!utf8 || size <= 0) {
      return;
    }

    const auto chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, nullptr, 0);
    if (chars <= 0) {
      return;
    }

    std::vector<WCHAR> wide(static_cast<std::size_t>(chars));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, wide.data(), chars) <= 0) {
      return;
    }

    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);
    for (const auto release : {false, true}) {
      for (const auto ch : wide) {
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = ch;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (release ? KEYEVENTF_KEYUP : 0);
        inputs.push_back(input);
      }
    }
    send(std::span<INPUT> {inputs});
  }
}  // namespace platf::session_input
