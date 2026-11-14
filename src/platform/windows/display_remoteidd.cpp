/**
 * @file src/platform/windows/display_remoteidd.cpp
 * @brief Definitions for the OpenStreamMS RemoteIdd capture backend.
 * @note snapshot() lives in display_vram.cpp, next to the other VRAM backends that share
 *       its image helpers.
 */
// standard includes
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "display.h"
#include "misc.h"
#include "remoteidd/FrameConsumer.h"
#include "src/config.h"
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

    /**
     * @brief osms-mosaic.txt, written by OpenStreamMS next to this instance's sunshine.conf.
     *
     * One line per session: "<Terminal Services session id> <part>", where part is the
     * same 2x2 grid name OpenStreamMS uses for split seats (Full, Left, Right, Top, Bottom,
     * TopLeft, TopRight, BottomLeft, BottomRight). Lines starting with # are comments.
     */
    std::filesystem::path mosaic_path() {
      return std::filesystem::path(config::sunshine.config_file).parent_path() / "osms-mosaic.txt";
    }

    /**
     * @brief Whole content of osms-mosaic.txt, or empty when there is none.
     */
    std::string read_layout() {
      std::ifstream file(mosaic_path(), std::ios::binary);
      if (!file) {
        return {};
      }
      std::ostringstream content;
      content << file.rdbuf();
      return content.str();
    }

    /**
     * @brief Cells of the 2x2 grid covered by a part: 1 top-left, 2 top-right, 4 bottom-left, 8 bottom-right.
     * @return 0 for an unknown name.
     */
    int area_cells(std::string_view area) {
      constexpr std::pair<std::string_view, int> areas[] {
        {"Full"sv, 0b1111},
        {"Left"sv, 0b0101},
        {"Right"sv, 0b1010},
        {"Top"sv, 0b0011},
        {"Bottom"sv, 0b1100},
        {"TopLeft"sv, 0b0001},
        {"TopRight"sv, 0b0010},
        {"BottomLeft"sv, 0b0100},
        {"BottomRight"sv, 0b1000},
      };
      for (const auto &[name, cells] : areas) {
        if (name == area) {
          return cells;
        }
      }
      return 0;
    }

    /**
     * @brief A session listed in osms-mosaic.txt.
     */
    struct mosaic_entry_t {
      uint32_t session;  ///< Terminal Services session id.
      int cells;  ///< Grid cells it covers.
    };

    /**
     * @brief Parse osms-mosaic.txt. Lines that do not parse, and parts that overlap an
     * earlier line, are skipped.
     */
    std::vector<mosaic_entry_t> parse_layout(const std::string &text) {
      std::vector<mosaic_entry_t> entries;
      int used = 0;
      std::istringstream lines(text);
      std::string line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
          continue;
        }
        std::istringstream fields(line);
        uint32_t session = 0;
        std::string area;
        if (!(fields >> session >> area)) {
          continue;
        }
        int cells = area_cells(area);
        if (!cells || (cells & used)) {
          BOOST_LOG(warning) << "RemoteIdd mosaic: skipping line \""sv << line << '"';
          continue;
        }
        used |= cells;
        entries.push_back({session, cells});
      }
      return entries;
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

    layout = read_layout();
    next_layout_check = std::chrono::steady_clock::now() + 250ms;
    auto entries = parse_layout(layout);
    bool listed = std::ranges::any_of(entries, [&](const mosaic_entry_t &e) {
      return e.session == session;
    });

    if (entries.size() < 2 || !listed) {
      consumer = std::make_unique<osms::ridd::FrameConsumer>(Microsoft::WRL::ComPtr<ID3D11Device>(device.get()), session, ns.c_str());
      if (!consumer->Open()) {
        BOOST_LOG(error) << "RemoteIdd: no frame slot for session "sv << session << "; is the driver running?"sv;
        return -1;
      }
      BOOST_LOG(info) << "RemoteIdd: reading frames for session "sv << session;
      return 0;
    }

    // Mosaic: the stream is a canvas of the size the client asked for, split like the
    // grid; OpenStreamMS reconnects each session to the size of its part.
    const int canvas_w = config.width;
    const int canvas_h = config.height;
    if (canvas_w <= 0 || canvas_h <= 0) {
      BOOST_LOG(error) << "RemoteIdd mosaic: invalid stream size "sv << canvas_w << 'x' << canvas_h;
      return -1;
    }

    for (const auto &e : entries) {
      // Same split as SeatMonitor.Region in OpenStreamMS: with an odd size the left/top
      // part gets the smaller side.
      const bool left = e.cells & 0b0101;
      const bool right = e.cells & 0b1010;
      const bool top = e.cells & 0b0011;
      const bool bottom = e.cells & 0b1100;
      const int w1 = canvas_w / 2;
      const int h1 = canvas_h / 2;

      mosaic_tile_t tile;
      tile.session = e.session;
      tile.x = left ? 0 : w1;
      tile.width = left && right ? canvas_w : left ? w1 : canvas_w - w1;
      tile.y = top ? 0 : h1;
      tile.height = top && bottom ? canvas_h : top ? h1 : canvas_h - h1;

      if (e.session == session) {
        // Absolute mouse: the client points at the canvas, the desktop is this tile.
        offset_x -= tile.x;
        offset_y -= tile.y;
      }
      BOOST_LOG(info) << "RemoteIdd mosaic: session "sv << tile.session << " at "sv << tile.x << ',' << tile.y << ' ' << tile.width << 'x' << tile.height;
      tiles.push_back(std::move(tile));
    }

    width = width_before_rotation = canvas_w;
    height = height_before_rotation = canvas_h;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = canvas_w;
    desc.Height = canvas_h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = capture_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *raw_canvas = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &raw_canvas))) {
      BOOST_LOG(error) << "RemoteIdd mosaic: failed to create the canvas"sv;
      return -1;
    }
    canvas.reset(raw_canvas);
    ID3D11RenderTargetView *raw_rt = nullptr;
    if (FAILED(device->CreateRenderTargetView(canvas.get(), nullptr, &raw_rt))) {
      BOOST_LOG(error) << "RemoteIdd mosaic: failed to create the canvas render target"sv;
      return -1;
    }
    canvas_rt.reset(raw_rt);
    clear_canvas();

    for (auto &tile : tiles) {
      open_tile(tile);
    }
    BOOST_LOG(info) << "RemoteIdd mosaic: "sv << tiles.size() << " sessions on a "sv << canvas_w << 'x' << canvas_h << " canvas"sv;
    return 0;
  }

  bool display_ridd_vram_t::layout_changed() {
    auto now = std::chrono::steady_clock::now();
    if (now < next_layout_check) {
      return false;
    }
    next_layout_check = now + 250ms;
    if (read_layout() == layout) {
      return false;
    }
    BOOST_LOG(info) << "RemoteIdd: mosaic layout changed, reinitializing capture"sv;
    return true;
  }

  bool display_ridd_vram_t::open_tile(mosaic_tile_t &tile) {
    auto ns = ridd_namespace();
    auto reader = std::make_unique<osms::ridd::FrameConsumer>(Microsoft::WRL::ComPtr<ID3D11Device>(device.get()), tile.session, ns.c_str());
    if (!reader->Open()) {
      // The session may still be connecting: try again shortly.
      tile.next_open = std::chrono::steady_clock::now() + 500ms;
      return false;
    }
    tile.consumer = std::move(reader);
    tile.last_frame = 0;
    compose_now = true;
    BOOST_LOG(info) << "RemoteIdd mosaic: reading frames for session "sv << tile.session;
    return true;
  }

  void display_ridd_vram_t::clear_canvas() {
    constexpr float black[4] {0.0f, 0.0f, 0.0f, 1.0f};
    device_ctx->ClearRenderTargetView(canvas_rt.get(), black);
    for (auto &tile : tiles) {
      tile.last_frame = 0;
    }
    compose_now = true;
  }
}  // namespace platf::dxgi
