// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace contour::display
{

/// What a caller of FrameRateLimiter::request() must do with the frame it asked for.
enum class RedrawAction : std::uint8_t
{
    Draw = 0, ///< The frame budget allows it: draw now.
    Defer,    ///< Too soon after the frame drawn last: draw once @ref RedrawTiming::delay has elapsed.
};

/// When a requested frame may draw.
struct RedrawTiming
{
    /// Whether the frame draws now or once @ref delay has elapsed.
    RedrawAction action = RedrawAction::Draw;

    /// The remaining wait, anchored to the frame drawn last. Zero when @ref action is Draw.
    std::chrono::steady_clock::duration delay {};
};

/// The shortest interval between two frames of @p maxFramesPerSecond frames per second.
/// @param maxFramesPerSecond The configured frame budget; zero or less means no limit.
/// @return The interval, truncated to whole nanoseconds, or zero when there is no limit.
[[nodiscard]] constexpr std::chrono::steady_clock::duration minFrameInterval(int maxFramesPerSecond) noexcept
{
    if (maxFramesPerSecond <= 0)
        return std::chrono::steady_clock::duration::zero();

    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds { 1'000'000'000 } / maxFramesPerSecond);
}

/// Caps how often output-driven redraws may draw, so a flood of output cannot burn a core per window.
///
/// A frame draws the screen as it is when it renders, so asking for another one while the budget is
/// spent only needs a wait: the deferred frame draws every change made meanwhile. The decision is a pure
/// function of the clock the caller passes in -- there is no clock, timer or callback in here -- which is
/// what makes every rule below testable without a display.
///
/// The caller keeps the frame DUE, not this class: a deferred request names how long to wait, and a
/// caller that waits that long draws the frame. Nothing is remembered about the request itself, so two
/// requests made in the same window defer to the same instant rather than to two instants.
///
/// Not thread-safe: one instance belongs to one display's GUI-thread frame path.
class FrameRateLimiter
{
  public:
    /// Records a redraw request and says whether it may draw now.
    ///
    /// The frame drawn last anchors the budget: a request less than @ref minFrameInterval away from it is
    /// deferred by the remainder. A bypass granted by @ref admitNextFrame() is spent here, whether or not
    /// a limit is configured, so it can never outlive the frame it was meant for.
    /// @param now The current time, handed in by the caller.
    /// @param maxFramesPerSecond The frame budget; zero or less draws every request immediately.
    /// @return Draw, or Defer with the wait left in the window.
    [[nodiscard]] RedrawTiming request(std::chrono::steady_clock::time_point now,
                                       int maxFramesPerSecond) noexcept
    {
        auto const admitted = std::exchange(_nextFrameAdmitted, false);
        auto const interval = minFrameInterval(maxFramesPerSecond);
        // Whether the frame drawn last still holds the budget: only then does a request have to wait.
        auto const budgetHolds = _lastFrame.has_value() && now - *_lastFrame < interval;
        if (admitted || interval == std::chrono::steady_clock::duration::zero() || !budgetHolds)
        {
            _lastFrame = now;
            return {};
        }

        // A steady clock does not run backwards, but the caller's time point can still be earlier than
        // the frame drawn last (a caller that keeps its own notion of "now"). Clamped, so such a reading
        // waits out one window rather than an unbounded one.
        auto const remaining = (*_lastFrame + interval) - now;
        return RedrawTiming {
            .action = RedrawAction::Defer,
            .delay = remaining < interval ? remaining : interval,
        };
    }

    /// Lets the next request draw immediately, without spending the budget it would otherwise wait for.
    ///
    /// For the frames a rate limit must not delay: the echo of the user's own keystrokes, and the frame a
    /// timer that has just come due asked for.
    void admitNextFrame() noexcept { _nextFrameAdmitted = true; }

    /// Forgets the frame drawn last, so the next request draws immediately: a display starts over with
    /// every session it is bound to, and its new owner's first frame is not the old one's to pace.
    void reset() noexcept
    {
        _lastFrame = std::nullopt;
        _nextFrameAdmitted = false;
    }

  private:
    /// When the last frame drew, or no value before the first one (and after @ref reset).
    std::optional<std::chrono::steady_clock::time_point> _lastFrame {};

    /// Whether the next request draws regardless of the budget; spent by @ref request.
    bool _nextFrameAdmitted = false;
};

} // namespace contour::display
