/**
 * @file src/platform/windows/display_remoteidd.cpp
 * @brief Definitions for the OpenStreamMS RemoteIdd capture backend.
 * @note snapshot() lives in display_vram.cpp, next to the other VRAM backends that share
 *       its image helpers.
 */
// standard includes
#include <cstdlib>
#include <iterator>
#include <string>

// local includes
#include "display.h"
#include "misc.h"
#include "remoteidd/FrameConsumer.h"
#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    /**
     * @brief Read a process environment variable (Win32 view, not the CRT copy).
     */
    std::wstring env(const wchar_t *name) {
      wchar_t buf[64];
      DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
      return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
    }

    /**
     * @brief Namespace prefix of the RemoteIdd named objects.
     *
     * The driver publishes in Global\. OSMS_RIDD_NS lets tests use Local\ without privileges.
     */
    std::wstring ridd_namespace() {
      if (auto ns = env(L"OSMS_RIDD_NS"); !ns.empty()) {
        return ns;
      }
      return REMOTEIDD_NS;
    }

    /**
     * @brief Terminal Services session whose slot this process reads.
     *
     * Sunshine runs inside the seat session; OSMS_RIDD_SESSION overrides it for tests.
     */
    uint32_t ridd_session() {
      if (auto s = env(L"OSMS_RIDD_SESSION"); !s.empty()) {
        return static_cast<uint32_t>(std::wcstoul(s.c_str(), nullptr, 10));
      }
      DWORD session = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      return session;
    }
  }  // namespace

  // Out of line: FrameConsumer is incomplete in display.h.
  display_ridd_vram_t::display_ridd_vram_t() = default;
  display_ridd_vram_t::~display_ridd_vram_t() = default;

  int display_ridd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (config.dynamicRange) {
      BOOST_LOG(error) << "RemoteIdd capture does not support HDR yet"sv;
      return -1;
    }
    if (display_base_t::init(config, display_name)) {
      return -1;
    }

    // IddCx 1.4 swapchains are always 8-bit BGRA.
    capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    auto ns = ridd_namespace();
    auto session = ridd_session();
    consumer = std::make_unique<osms::ridd::FrameConsumer>(Microsoft::WRL::ComPtr<ID3D11Device>(device.get()), session, ns.c_str());
    if (!consumer->Open()) {
      BOOST_LOG(error) << "RemoteIdd: no frame slot for session "sv << session << "; is the driver running?"sv;
      return -1;
    }
    BOOST_LOG(info) << "RemoteIdd: reading frames for session "sv << session;
    return 0;
  }
}  // namespace platf::dxgi
