/**
 * @file src/platform/windows/frame_rate_limiter.h
 * @brief Declarations for the arrival-driven capture rate limiter.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

namespace platf::dxgi {
  /**
   * @brief Passes captured frames as they arrive while capping their average rate.
   *
   * @details When the display composes much faster than the client frame rate, sampling it on a
   * fixed client-rate grid loses frames: content updates land on display ticks, so a grid slot can
   * fall just before the next update and come back empty. This limiter instead accepts each new
   * frame when it arrives, as long as it is not earlier than its slot minus a tolerance of one
   * display tick. Slots advance by one client frame period per accepted frame, so the long-run
   * rate never exceeds the client frame rate. After a stall longer than one period the schedule is
   * re-anchored on the next frame.
   */
  class frame_rate_limiter_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock used for frame timestamps.

    /**
     * @brief Create a limiter.
     *
     * @param period Client frame period: the minimum average spacing between accepted frames.
     * @param tolerance How much earlier than its slot a frame may still be accepted.
     */
    frame_rate_limiter_t(std::chrono::nanoseconds period, std::chrono::nanoseconds tolerance):
        period_ {period},
        tolerance_ {tolerance} {
    }

    /**
     * @brief Decide whether a frame that became available at @p timestamp should be sent.
     *
     * @param timestamp Time the frame was composed.
     * @return True when the frame should be sent; the next slot is then advanced.
     */
    bool accept(clock::time_point timestamp) {
      if (!next_slot_ || timestamp > *next_slot_ + period_) {
        next_slot_ = timestamp + period_;
        return true;
      }
      if (timestamp + tolerance_ >= *next_slot_) {
        *next_slot_ += period_;
        return true;
      }
      return false;
    }

    /**
     * @brief Time from which the next frame will be accepted.
     *
     * @return The earliest accepted timestamp, or no value before the first frame.
     */
    [[nodiscard]] std::optional<clock::time_point> next_accept_time() const {
      if (!next_slot_) {
        return std::nullopt;
      }
      return *next_slot_ - tolerance_;
    }

    /**
     * @brief Forget the schedule; the next frame is accepted and re-anchors it.
     */
    void reset() {
      next_slot_.reset();
    }

  private:
    std::chrono::nanoseconds period_;  ///< Client frame period.
    std::chrono::nanoseconds tolerance_;  ///< Early-arrival tolerance.
    std::optional<clock::time_point> next_slot_;  ///< Slot of the next frame to send.
  };
}  // namespace platf::dxgi
