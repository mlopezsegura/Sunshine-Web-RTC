/**
 * @file tests/unit/platform/windows/test_frame_rate_limiter.cpp
 * @brief Test the arrival-driven capture rate limiter.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <chrono>
  #include <cmath>

  // local includes
  #include <src/platform/windows/frame_rate_limiter.h>

using namespace std::chrono_literals;
using limiter_t = platf::dxgi::frame_rate_limiter_t;

namespace {
  constexpr std::chrono::nanoseconds period_60 {16'666'667};
  constexpr std::chrono::nanoseconds tick_125 {8'000'000};

  /**
   * @brief Count the frames a limiter accepts when content is presented at @p content_period and
   * composed on a @p tick grid, over @p duration.
   */
  int accepted_frames(std::chrono::nanoseconds content_period, std::chrono::nanoseconds tick, std::chrono::nanoseconds duration) {
    limiter_t limiter {period_60, tick};
    const auto origin = limiter_t::clock::time_point {} + 1h;
    int accepted = 0;
    for (std::chrono::nanoseconds present {0}; present < duration; present += content_period) {
      const auto ticks = (present.count() + tick.count() - 1) / tick.count();
      if (limiter.accept(origin + tick * ticks)) {
        ++accepted;
      }
    }
    return accepted;
  }
}  // namespace

/**
 * @brief The first frame is always accepted.
 */
TEST(FrameRateLimiterTests, AcceptsFirstFrame) {
  limiter_t limiter {period_60, tick_125};
  EXPECT_FALSE(limiter.next_accept_time());
  EXPECT_TRUE(limiter.accept(limiter_t::clock::now()));
  EXPECT_TRUE(limiter.next_accept_time());
}

/**
 * @brief A frame earlier than its slot minus the tolerance is rejected, and one inside it is not.
 */
TEST(FrameRateLimiterTests, RejectsFramesBeforeTheirSlot) {
  limiter_t limiter {period_60, tick_125};
  const auto start = limiter_t::clock::now();
  ASSERT_TRUE(limiter.accept(start));
  EXPECT_EQ(*limiter.next_accept_time(), start + period_60 - tick_125);
  EXPECT_FALSE(limiter.accept(start + 8ms));
  EXPECT_TRUE(limiter.accept(start + 9ms));
}

/**
 * @brief After a stall longer than one period the schedule restarts at the next frame.
 */
TEST(FrameRateLimiterTests, ReanchorsAfterStall) {
  limiter_t limiter {period_60, tick_125};
  const auto start = limiter_t::clock::now();
  ASSERT_TRUE(limiter.accept(start));
  const auto late = start + 100ms;
  EXPECT_TRUE(limiter.accept(late));
  EXPECT_EQ(*limiter.next_accept_time(), late + period_60 - tick_125);
}

/**
 * @brief Reset forgets the schedule.
 */
TEST(FrameRateLimiterTests, ResetForgetsSchedule) {
  limiter_t limiter {period_60, tick_125};
  const auto start = limiter_t::clock::now();
  ASSERT_TRUE(limiter.accept(start));
  limiter.reset();
  EXPECT_FALSE(limiter.next_accept_time());
  EXPECT_TRUE(limiter.accept(start + 1ms));
}

/**
 * @brief Content capped at 60 fps and composed at 125 Hz passes every frame.
 */
TEST(FrameRateLimiterTests, PassesCappedContentOn125HzComposition) {
  EXPECT_EQ(accepted_frames(period_60, tick_125, 10s), 600);
}

/**
 * @brief Content at 62.5 fps composed at 125 Hz is held to about 60 frames per second.
 */
TEST(FrameRateLimiterTests, CapsFasterContentToClientRate) {
  const int accepted = accepted_frames(16ms, tick_125, 10s);
  EXPECT_LE(accepted, 601);
  EXPECT_GE(accepted, 595);
}

/**
 * @brief Content slower than the client rate passes unchanged.
 */
TEST(FrameRateLimiterTests, PassesSlowerContent) {
  EXPECT_EQ(accepted_frames(20ms, tick_125, 10s), 500);
}
#endif
