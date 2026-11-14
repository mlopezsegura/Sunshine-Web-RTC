/**
 * @file tests/unit/platform/windows/test_remoteidd.cpp
 * @brief Test the OpenStreamMS RemoteIdd frame hand-off, capture backend and mosaic.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <filesystem>
  #include <fstream>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <dxgi1_2.h>

  // local includes
  #include "remoteidd/FramePublisher.h"

  #include <src/config.h>
  #include <src/logging.h>
  #include <src/platform/windows/display.h>
  #include <src/platform/windows/remoteidd/FrameConsumer.h>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace {
  constexpr const wchar_t *test_ns = L"Local\\";

  bool create_device(ComPtr<ID3D11Device> &dev, ComPtr<ID3D11DeviceContext> &ctx) {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter))) {
      return false;
    }
    return SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
  }

  /**
   * @brief Source frames as an IddCx swapchain would hand them to the driver.
   */
  struct frame_source_t {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;

    bool init(ID3D11Device *dev, UINT width, UINT height) {
      D3D11_TEXTURE2D_DESC d {};
      d.Width = width;
      d.Height = height;
      d.MipLevels = d.ArraySize = 1;
      d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &texture)) && SUCCEEDED(dev->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
    }

    // The color encodes the frame id: B8G8R8A8_UNORM stores n/255 back as the byte n.
    void paint(ID3D11DeviceContext *ctx, UINT64 id) const {
      const float rgba[4] = {static_cast<float>(id & 0xFF) / 255.0f, static_cast<float>((id >> 8) & 0xFF) / 255.0f, static_cast<float>((id >> 16) & 0xFF) / 255.0f, 1.0f};
      ctx->ClearRenderTargetView(rtv.Get(), rgba);
    }
  };

  UINT64 read_center_id(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC sd {};
    sd.Width = sd.Height = 1;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
      return 0;
    }
    D3D11_BOX box {width / 2, height / 2, 0, width / 2 + 1, height / 2 + 1, 1};
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
      return 0;
    }
    auto px = static_cast<const BYTE *>(m.pData);
    UINT64 id = px[2] | (px[1] << 8) | (px[0] << 16);
    ctx->Unmap(staging.Get(), 0);
    return id;
  }

  /**
   * @brief Sleep until @p deadline with a high-resolution waitable timer.
   *
   * std::this_thread::sleep_until rounds to the ~15.6 ms system tick and would turn a
   * 240 Hz producer into bursts; this does not change the global timer resolution.
   */
  void sleep_until_precise(std::chrono::steady_clock::time_point deadline) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
      auto left = deadline - std::chrono::steady_clock::now();
      if (left <= 0ns) {
        return;
      }
      if (timer && left > 500us) {
        LARGE_INTEGER due;
        due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(left - 300us).count() / 100;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
      } else {
        YieldProcessor();
      }
    }
  }

  UINT32 unique_session(UINT32 offset) {
    return 90000 + (GetCurrentProcessId() % 1000) * 10 + offset;
  }
}  // namespace

/**
 * @brief Every published frame reaches the consumer with the right pixels, across devices.
 */
TEST(RemoteIddHandOffTest, DeliversFrameContentAcrossDevices) {
  ComPtr<ID3D11Device> pdev, cdev;
  ComPtr<ID3D11DeviceContext> pctx, cctx;
  if (!create_device(pdev, pctx) || !create_device(cdev, cctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto session = unique_session(0);
  osms::ridd::FramePublisher publisher(pdev, pctx, session, test_ns, nullptr);
  osms::ridd::FrameConsumer consumer(cdev, session, test_ns);
  frame_source_t src;
  ASSERT_TRUE(src.init(pdev.Get(), 1280, 720));

  for (UINT64 i = 1; i <= 30; ++i) {
    src.paint(pctx.Get(), publisher.LastFrameId() + 1);
    ASSERT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
    if (i == 1) {
      ASSERT_TRUE(consumer.Open());
    }
    ASSERT_EQ(consumer.WaitFrame(1000), osms::ridd::FrameConsumer::Wait::Frame);

    ComPtr<ID3D11Texture2D> tex;
    RemoteIddFrameInfo frame;
    ASSERT_TRUE(consumer.Acquire(1000, &tex, &frame));
    EXPECT_EQ(frame.FrameId, i);
    EXPECT_EQ(frame.Width, 1280u);
    EXPECT_EQ(read_center_id(cdev.Get(), cctx.Get(), tex.Get(), frame.Width, frame.Height), i);
    consumer.Release();
  }
}

/**
 * @brief A resize recreates the shared texture and the consumer follows the new generation.
 */
TEST(RemoteIddHandOffTest, FollowsTextureRecreationOnResize) {
  ComPtr<ID3D11Device> pdev, cdev;
  ComPtr<ID3D11DeviceContext> pctx, cctx;
  if (!create_device(pdev, pctx) || !create_device(cdev, cctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto session = unique_session(1);
  osms::ridd::FramePublisher publisher(pdev, pctx, session, test_ns, nullptr);
  osms::ridd::FrameConsumer consumer(cdev, session, test_ns);

  const UINT sizes[][2] = {{1920, 1080}, {1280, 720}, {3840, 2160}};
  UINT32 last_generation = 0;
  for (const auto &size : sizes) {
    frame_source_t src;
    ASSERT_TRUE(src.init(pdev.Get(), size[0], size[1]));
    src.paint(pctx.Get(), publisher.LastFrameId() + 1);
    ASSERT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
    ASSERT_TRUE(consumer.Open());
    ASSERT_EQ(consumer.WaitFrame(1000), osms::ridd::FrameConsumer::Wait::Frame);

    ComPtr<ID3D11Texture2D> tex;
    RemoteIddFrameInfo frame;
    ASSERT_TRUE(consumer.Acquire(1000, &tex, &frame));
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    EXPECT_EQ(desc.Width, size[0]);
    EXPECT_EQ(desc.Height, size[1]);
    EXPECT_GT(frame.TextureGeneration, last_generation);
    EXPECT_TRUE(frame.Flags & REMOTEIDD_FLAG_TEXTURE_CHANGED);
    EXPECT_EQ(read_center_id(cdev.Get(), cctx.Get(), tex.Get(), desc.Width, desc.Height), frame.FrameId);
    last_generation = frame.TextureGeneration;
    consumer.Release();
  }
}

/**
 * @brief While the consumer holds the texture, publishing drops the frame instead of blocking.
 */
TEST(RemoteIddHandOffTest, PublishNeverBlocksOnBusyConsumer) {
  ComPtr<ID3D11Device> pdev, cdev;
  ComPtr<ID3D11DeviceContext> pctx, cctx;
  if (!create_device(pdev, pctx) || !create_device(cdev, cctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto session = unique_session(2);
  osms::ridd::FramePublisher publisher(pdev, pctx, session, test_ns, nullptr);
  osms::ridd::FrameConsumer consumer(cdev, session, test_ns);
  frame_source_t src;
  ASSERT_TRUE(src.init(pdev.Get(), 1920, 1080));

  ASSERT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
  ASSERT_TRUE(consumer.Open());
  ASSERT_EQ(consumer.WaitFrame(1000), osms::ridd::FrameConsumer::Wait::Frame);
  ComPtr<ID3D11Texture2D> tex;
  RemoteIddFrameInfo frame;
  ASSERT_TRUE(consumer.Acquire(1000, &tex, &frame));

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Dropped);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 20ms) << "20 publishes against a held texture must not wait";
  consumer.Release();

  EXPECT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
}

/**
 * @brief The capture backend delivers a producer's frames at 240 Hz with low added latency.
 */
TEST(RemoteIddCaptureTest, CapturesPublishedFramesAt240Hz) {
  ComPtr<ID3D11Device> pdev;
  ComPtr<ID3D11DeviceContext> pctx;
  if (!create_device(pdev, pctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto session = unique_session(3);
  osms::ridd::FramePublisher publisher(pdev, pctx, session, test_ns, nullptr);

  // The backend opens the slot during init, so the producer has to exist first.
  frame_source_t boot;
  ASSERT_TRUE(boot.init(pdev.Get(), 64, 64));
  ASSERT_EQ(publisher.Publish(boot.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);

  SetEnvironmentVariableW(L"OSMS_RIDD_NS", test_ns);
  SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", std::to_wstring(session).c_str());
  auto restore_capture = config::video.capture;
  config::video.capture = "remoteidd";
  auto cleanup = util::fail_guard([&]() {
    config::video.capture = restore_capture;
    SetEnvironmentVariableW(L"OSMS_RIDD_NS", nullptr);
    SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", nullptr);
  });

  video::config_t config {1920, 1080, 240, 0, 20000, 1, 1, 0, 1, 0, 0, 0};
  auto disp = std::dynamic_pointer_cast<platf::dxgi::display_ridd_vram_t>(platf::display(platf::mem_type_e::dxgi, "", config));
  if (!disp) {
    GTEST_SKIP() << "No display output to initialize the capture backend";
  }

  const UINT width = disp->width_before_rotation;
  const UINT height = disp->height_before_rotation;
  frame_source_t src;
  ASSERT_TRUE(src.init(pdev.Get(), width, height));
  // Leave a frame of the display size in the slot so the first snapshot does not see the boot one.
  src.paint(pctx.Get(), publisher.LastFrameId() + 1);
  ASSERT_EQ(publisher.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);

  constexpr int frames = 480;  // 2 s at 240 Hz
  std::atomic<bool> producing {true};
  std::thread producer([&]() {
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < frames && producing; ++i) {
      sleep_until_precise(next);
      next += std::chrono::nanoseconds(1'000'000'000 / 240);
      src.paint(pctx.Get(), publisher.LastFrameId() + 1);
      LARGE_INTEGER qpc;
      QueryPerformanceCounter(&qpc);
      publisher.Publish(src.texture.Get(), static_cast<UINT64>(qpc.QuadPart));
    }
    producing = false;
  });

  std::vector<std::shared_ptr<platf::img_t>> pool;
  auto pull = [&](std::shared_ptr<platf::img_t> &out) {
    auto it = std::find_if(pool.begin(), pool.end(), [](auto &img) {
      return img.use_count() == 1;
    });
    if (it == pool.end()) {
      pool.push_back(disp->alloc_img());
      it = pool.end() - 1;
    }
    out = *it;
    return true;
  };

  int captured = 0, reinit = 0;
  std::vector<double> age_ms;
  while (producing || captured == 0) {
    std::shared_ptr<platf::img_t> img;
    auto status = disp->snapshot(pull, img, 100ms, false);
    if (status == platf::capture_e::ok) {
      ++captured;
      ASSERT_TRUE(img->frame_timestamp.has_value());
      age_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *img->frame_timestamp).count());
      disp->release_snapshot();
    } else if (status == platf::capture_e::reinit) {
      ++reinit;
    } else if (status != platf::capture_e::timeout) {
      ADD_FAILURE() << "snapshot failed: " << static_cast<int>(status);
      break;
    }
  }
  producing = false;
  producer.join();

  ASSERT_FALSE(age_ms.empty());
  std::sort(age_ms.begin(), age_ms.end());
  const double p50 = age_ms[age_ms.size() / 2];
  const double p99 = age_ms[std::min(age_ms.size() - 1, age_ms.size() * 99 / 100)];
  BOOST_LOG(info) << "RemoteIdd capture: " << captured << '/' << frames << " frames at " << width << 'x' << height
                  << ", present->snapshot (CPU) p50 " << p50 << " ms, p99 " << p99 << " ms";

  EXPECT_EQ(reinit, 0);
  EXPECT_GE(captured, frames * 95 / 100);
  EXPECT_LE(p99, 4.0);
}

/**
 * @brief Part names map to the cells of the 2x2 grid; unknown names to none.
 */
TEST(RemoteIddMosaicLayoutTest, MapsPartNamesToCells) {
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("Full"), 0b1111);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("Left"), 0b0101);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("Right"), 0b1010);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("Top"), 0b0011);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("Bottom"), 0b1100);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("TopLeft"), 0b0001);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("TopRight"), 0b0010);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("BottomLeft"), 0b0100);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("BottomRight"), 0b1000);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells("left"), 0);
  EXPECT_EQ(platf::dxgi::mosaic_area_cells(""), 0);
}

/**
 * @brief The parser reads the canvas and the sessions and skips comments, bad lines and overlaps.
 */
TEST(RemoteIddMosaicLayoutTest, ParsesCanvasSessionsAndSkipsBadLines) {
  auto layout = platf::dxgi::parse_mosaic_layout(
    "# OpenStreamMS mosaic\r\n"
    "canvas 1920 1080\r\n"
    "7 TopLeft\r\n"
    "\r\n"
    "9 TopRight\n"
    "11 Left\n"  // overlaps 7
    "x12 BottomLeft\n"  // not a session id
    "13 Middle\n"  // unknown part
    "14\n"  // no part
    "15 BottomRight\n"
  );
  EXPECT_EQ(layout.canvas_width, 1920);
  EXPECT_EQ(layout.canvas_height, 1080);
  ASSERT_EQ(layout.entries.size(), 3u);
  EXPECT_EQ(layout.entries[0].session, 7u);
  EXPECT_EQ(layout.entries[0].cells, 0b0001);
  EXPECT_EQ(layout.entries[1].session, 9u);
  EXPECT_EQ(layout.entries[1].cells, 0b0010);
  EXPECT_EQ(layout.entries[2].session, 15u);
  EXPECT_EQ(layout.entries[2].cells, 0b1000);
}

/**
 * @brief Without a valid canvas line the canvas is left to the stream size.
 */
TEST(RemoteIddMosaicLayoutTest, IgnoresInvalidCanvas) {
  EXPECT_EQ(platf::dxgi::parse_mosaic_layout("canvas 0 1080\n7 Left\n").canvas_width, 0);
  EXPECT_EQ(platf::dxgi::parse_mosaic_layout("canvas 1920\n7 Left\n").canvas_width, 0);
  EXPECT_EQ(platf::dxgi::parse_mosaic_layout("canvas a b\n7 Left\n").canvas_width, 0);
  EXPECT_TRUE(platf::dxgi::parse_mosaic_layout("").entries.empty());
}

/**
 * @brief Parts split the canvas like SeatMonitor.Region: the left/top half gets the smaller side.
 */
TEST(RemoteIddMosaicLayoutTest, SplitsCanvasIntoParts) {
  auto check = [](int cells, int x, int y, int w, int h) {
    auto r = platf::dxgi::mosaic_rect(cells, 1921, 1081);
    EXPECT_EQ(r.x, x) << cells;
    EXPECT_EQ(r.y, y) << cells;
    EXPECT_EQ(r.width, w) << cells;
    EXPECT_EQ(r.height, h) << cells;
  };
  check(0b1111, 0, 0, 1921, 1081);
  check(0b0101, 0, 0, 960, 1081);
  check(0b1010, 960, 0, 961, 1081);
  check(0b0011, 0, 0, 1921, 540);
  check(0b1100, 0, 540, 1921, 541);
  check(0b0001, 0, 0, 960, 540);
  check(0b0010, 960, 0, 961, 540);
  check(0b0100, 0, 540, 960, 541);
  check(0b1000, 960, 540, 961, 541);
}

namespace {
  /**
   * @brief Read the frame id painted at a point of a texture (see frame_source_t::paint).
   */
  UINT64 read_id_at(ID3D11Texture2D *tex, UINT x, UINT y) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    tex->GetDevice(&dev);
    dev->GetImmediateContext(&ctx);
    return read_center_id(dev.Get(), ctx.Get(), tex, x * 2, y * 2);
  }

  /**
   * @brief A temporary sunshine.conf location holding osms-mosaic.txt, restored on destruction.
   */
  struct mosaic_dir_t {
    std::filesystem::path dir;
    std::string restore = config::sunshine.config_file;

    mosaic_dir_t() {
      dir = std::filesystem::temp_directory_path() / ("sunshine-mosaic-test-" + std::to_string(GetCurrentProcessId()));
      std::filesystem::create_directories(dir);
      config::sunshine.config_file = (dir / "sunshine.conf").string();
    }

    ~mosaic_dir_t() {
      config::sunshine.config_file = restore;
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }

    void write(const std::string &text) const {
      std::ofstream(dir / "osms-mosaic.txt", std::ios::binary | std::ios::trunc) << text;
    }
  };
}  // namespace

/**
 * @brief In mosaic mode every session lands in its part of the canvas, a session that stops
 * presenting keeps its last frame, and the mouse is offset to the own part.
 */
TEST(RemoteIddCaptureTest, ComposesSessionsIntoMosaic) {
  ComPtr<ID3D11Device> pdev;
  ComPtr<ID3D11DeviceContext> pctx;
  if (!create_device(pdev, pctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto host = unique_session(4);
  const auto guest = unique_session(5);
  osms::ridd::FramePublisher host_pub(pdev, pctx, host, test_ns, nullptr);
  osms::ridd::FramePublisher guest_pub(pdev, pctx, guest, test_ns, nullptr);
  frame_source_t host_src;
  frame_source_t guest_src;
  ASSERT_TRUE(host_src.init(pdev.Get(), 64, 64));
  ASSERT_TRUE(guest_src.init(pdev.Get(), 64, 64));
  host_src.paint(pctx.Get(), 11);
  guest_src.paint(pctx.Get(), 22);
  ASSERT_EQ(host_pub.Publish(host_src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
  ASSERT_EQ(guest_pub.Publish(guest_src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);

  mosaic_dir_t files;

  SetEnvironmentVariableW(L"OSMS_RIDD_NS", test_ns);
  SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", std::to_wstring(host).c_str());
  auto restore_capture = config::video.capture;
  config::video.capture = "remoteidd";
  auto cleanup = util::fail_guard([&]() {
    config::video.capture = restore_capture;
    SetEnvironmentVariableW(L"OSMS_RIDD_NS", nullptr);
    SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", nullptr);
  });

  video::config_t config {1920, 1080, 60, 0, 20000, 1, 1, 0, 1, 0, 0, 0};
  auto plain = platf::display(platf::mem_type_e::dxgi, "", config);
  if (!plain) {
    GTEST_SKIP() << "No display output to initialize the capture backend";
  }
  const int plain_offset_x = plain->offset_x;
  const int plain_offset_y = plain->offset_y;
  plain.reset();

  files.write("canvas 128 64\n" + std::to_string(host) + " Right\n" + std::to_string(guest) + " Left\n");
  auto disp = std::dynamic_pointer_cast<platf::dxgi::display_ridd_vram_t>(platf::display(platf::mem_type_e::dxgi, "", config));
  ASSERT_TRUE(disp);
  ASSERT_NE(disp->canvas_texture(), nullptr);
  EXPECT_EQ(disp->width_before_rotation, 128);
  EXPECT_EQ(disp->height_before_rotation, 64);

  std::vector<std::shared_ptr<platf::img_t>> pool;
  auto pull = [&](std::shared_ptr<platf::img_t> &out) {
    pool.push_back(disp->alloc_img());
    out = pool.back();
    return true;
  };
  auto capture = [&]() {
    for (int i = 0; i < 20; ++i) {
      std::shared_ptr<platf::img_t> img;
      auto status = disp->snapshot(pull, img, 100ms, false);
      if (status == platf::capture_e::ok) {
        disp->release_snapshot();
        return true;
      }
      if (status != platf::capture_e::timeout) {
        ADD_FAILURE() << "snapshot failed: " << static_cast<int>(status);
        return false;
      }
    }
    return false;
  };

  // The first snapshot composes what is already published, without waiting for a present.
  ASSERT_TRUE(capture());
  EXPECT_EQ(read_id_at(disp->canvas_texture(), 32, 32), 22u);  // guest, left
  EXPECT_EQ(read_id_at(disp->canvas_texture(), 96, 32), 11u);  // host, right

  // Only the guest presents: the host keeps its last frame.
  guest_src.paint(pctx.Get(), 33);
  ASSERT_EQ(guest_pub.Publish(guest_src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);
  ASSERT_TRUE(capture());
  EXPECT_EQ(read_id_at(disp->canvas_texture(), 32, 32), 33u);
  EXPECT_EQ(read_id_at(disp->canvas_texture(), 96, 32), 11u);

  // Absolute mouse: canvas x 64 is the host desktop's left edge.
  EXPECT_EQ(disp->offset_x, plain_offset_x - 64);
  EXPECT_EQ(disp->offset_y, plain_offset_y);
}

/**
 * @brief Changing osms-mosaic.txt makes the backend ask for a reinit, in and out of mosaic mode.
 */
TEST(RemoteIddCaptureTest, ReinitializesWhenMosaicLayoutChanges) {
  ComPtr<ID3D11Device> pdev;
  ComPtr<ID3D11DeviceContext> pctx;
  if (!create_device(pdev, pctx)) {
    GTEST_SKIP() << "No D3D11 hardware device";
  }

  const auto host = unique_session(6);
  osms::ridd::FramePublisher host_pub(pdev, pctx, host, test_ns, nullptr);
  frame_source_t src;
  ASSERT_TRUE(src.init(pdev.Get(), 64, 64));
  ASSERT_EQ(host_pub.Publish(src.texture.Get(), 0), osms::ridd::FramePublisher::Result::Published);

  mosaic_dir_t files;  // no osms-mosaic.txt: single-session mode

  SetEnvironmentVariableW(L"OSMS_RIDD_NS", test_ns);
  SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", std::to_wstring(host).c_str());
  auto restore_capture = config::video.capture;
  config::video.capture = "remoteidd";
  auto cleanup = util::fail_guard([&]() {
    config::video.capture = restore_capture;
    SetEnvironmentVariableW(L"OSMS_RIDD_NS", nullptr);
    SetEnvironmentVariableW(L"OSMS_RIDD_SESSION", nullptr);
  });

  video::config_t config {1920, 1080, 60, 0, 20000, 1, 1, 0, 1, 0, 0, 0};
  auto disp = std::dynamic_pointer_cast<platf::dxgi::display_ridd_vram_t>(platf::display(platf::mem_type_e::dxgi, "", config));
  if (!disp) {
    GTEST_SKIP() << "No display output to initialize the capture backend";
  }
  EXPECT_EQ(disp->canvas_texture(), nullptr);

  // A guest that does not publish yet still turns the stream into a mosaic.
  files.write("canvas 128 64\n" + std::to_string(host) + " Left\n" + std::to_string(unique_session(7)) + " Right\n");
  std::this_thread::sleep_for(300ms);
  auto pull = [&](std::shared_ptr<platf::img_t> &out) {
    out = disp->alloc_img();
    return true;
  };
  std::shared_ptr<platf::img_t> img;
  EXPECT_EQ(disp->snapshot(pull, img, 10ms, false), platf::capture_e::reinit);

  auto mosaic = std::dynamic_pointer_cast<platf::dxgi::display_ridd_vram_t>(platf::display(platf::mem_type_e::dxgi, "", config));
  ASSERT_TRUE(mosaic);
  ASSERT_NE(mosaic->canvas_texture(), nullptr);

  // The guest slot does not exist: the host alone is composed, and nothing fails.
  std::shared_ptr<platf::img_t> first;
  EXPECT_EQ(mosaic->snapshot(pull, first, 100ms, false), platf::capture_e::ok);

  // Removing the file goes back to single-session mode.
  std::filesystem::remove(files.dir / "osms-mosaic.txt");
  std::this_thread::sleep_for(300ms);
  std::shared_ptr<platf::img_t> again;
  EXPECT_EQ(mosaic->snapshot(pull, again, 10ms, false), platf::capture_e::reinit);
}
#endif
