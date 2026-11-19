// SPDX-License-Identifier: Apache-2.0
#include <contour/display/FrameRateLimiter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

using contour::display::FrameRateLimiter;
using contour::display::minFrameInterval;
using contour::display::RedrawAction;
using contour::display::RedrawTiming;

namespace
{
/// 60 frames per second, the budget the software renderer is aimed at.
constexpr auto SixtyFps = 60;

/// An arbitrary origin; only the differences between time points mean anything here.
constexpr auto Start = std::chrono::steady_clock::time_point {};

/// @return Whether @p timing tells the caller to draw now.
[[nodiscard]] bool drawsNow(RedrawTiming const& timing)
{
    return timing.action == RedrawAction::Draw;
}
} // namespace

TEST_CASE("FrameRateLimiter.zeroDrawsEveryRequestImmediately", "[display][redraw][frame-rate]")
{
    // The default configuration: no limit at all, so the limiter must be unable to defer anything.
    auto limiter = FrameRateLimiter {};

    CHECK(drawsNow(limiter.request(Start, 0)));
    CHECK(drawsNow(limiter.request(Start, 0)));
    CHECK(drawsNow(limiter.request(Start + 1ns, 0)));
}

TEST_CASE("FrameRateLimiter.aNegativeLimitIsTreatedAsNoLimit", "[display][redraw][frame-rate]")
{
    // The loader clamps this, but a limiter that deferred on a nonsense value would be a worse answer
    // than one that draws: the setting can only ever make the display slower.
    auto limiter = FrameRateLimiter {};

    CHECK(drawsNow(limiter.request(Start, -1)));
    CHECK(drawsNow(limiter.request(Start, -1)));
}

TEST_CASE("FrameRateLimiter.theFirstFrameDrawsImmediately", "[display][redraw][frame-rate]")
{
    auto limiter = FrameRateLimiter {};

    auto const timing = limiter.request(Start, SixtyFps);

    CHECK(drawsNow(timing));
    CHECK(timing.delay == std::chrono::steady_clock::duration::zero());
}

TEST_CASE("FrameRateLimiter.aFrameInsideTheWindowWaitsOutTheRemainder", "[display][redraw][frame-rate]")
{
    auto const interval = minFrameInterval(SixtyFps);
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));

    auto const timing = limiter.request(Start + 5ms, SixtyFps);

    CHECK(timing.action == RedrawAction::Defer);
    CHECK(timing.delay == interval - 5ms);

    // Once the wait is over, the frame draws -- and it is the deferred one, not a second frame.
    CHECK(drawsNow(limiter.request(Start + 5ms + timing.delay, SixtyFps)));
    CHECK(limiter.request(Start + 5ms + timing.delay + 1ms, SixtyFps).action == RedrawAction::Defer);
}

TEST_CASE("FrameRateLimiter.aFrameExactlyOneIntervalLaterDraws", "[display][redraw][frame-rate]")
{
    auto const interval = minFrameInterval(SixtyFps);
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));

    CHECK(drawsNow(limiter.request(Start + interval, SixtyFps)));
}

TEST_CASE("FrameRateLimiter.everyRequestInOneWindowDefersToTheSameInstant", "[display][redraw][frame-rate]")
{
    // Nothing is lost by deferring: a second and a third request inside the window name the SAME instant,
    // which is what keeps a restarted timer from pushing the frame further out with every batch of output.
    auto const interval = minFrameInterval(SixtyFps);
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));

    auto const first = limiter.request(Start + 1ms, SixtyFps);
    auto const second = limiter.request(Start + 10ms, SixtyFps);

    REQUIRE(first.action == RedrawAction::Defer);
    REQUIRE(second.action == RedrawAction::Defer);
    CHECK(Start + 1ms + first.delay == Start + 10ms + second.delay);
    CHECK(first.delay == interval - 1ms);
    CHECK(second.delay == interval - 10ms);
}

TEST_CASE("FrameRateLimiter.anAdmittedFrameDrawsImmediately", "[display][redraw][frame-rate]")
{
    // The path a keystroke takes: its echo must not wait behind the output flood's budget.
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));
    REQUIRE(limiter.request(Start + 1ms, SixtyFps).action == RedrawAction::Defer);

    limiter.admitNextFrame();

    CHECK(drawsNow(limiter.request(Start + 1ms, SixtyFps)));
}

TEST_CASE("FrameRateLimiter.anAdmissionIsSpentByTheFrameItAdmits", "[display][redraw][frame-rate]")
{
    // Otherwise one keystroke would exempt every later frame, and a flood would draw unthrottled.
    auto const interval = minFrameInterval(SixtyFps);
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));
    limiter.admitNextFrame();
    REQUIRE(drawsNow(limiter.request(Start + 1ms, SixtyFps)));

    auto const timing = limiter.request(Start + 2ms, SixtyFps);

    CHECK(timing.action == RedrawAction::Defer);
    CHECK(timing.delay == interval - 1ms);

    // An admission made while no frame follows is spent by the next frame whenever that comes.
    limiter.admitNextFrame();
    CHECK(drawsNow(limiter.request(Start + 20 * interval, SixtyFps)));
}

TEST_CASE("FrameRateLimiter.anAdmissionUnderNoLimitIsSpentByTheFrameItAdmits",
          "[display][redraw][frame-rate]")
{
    // A display configured without a limit still takes keystrokes; the admission must not survive the
    // frame it was granted for and admit a later one once the user configures a limit.
    auto limiter = FrameRateLimiter {};
    limiter.admitNextFrame();
    REQUIRE(drawsNow(limiter.request(Start, 0)));

    // That frame drew, and it anchors the budget the next request is paced by -- the admission is gone,
    // so a limit configured right afterwards waits out its window like any other frame.
    CHECK(limiter.request(Start + 1ms, SixtyFps).action == RedrawAction::Defer);
}

TEST_CASE("FrameRateLimiter.aTimePointBeforeTheLastFrameWaitsOneWindow", "[display][redraw][frame-rate]")
{
    // A caller whose clock reads earlier than the frame drawn last must not be told to wait forever.
    auto const interval = minFrameInterval(SixtyFps);
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start + 10ms, SixtyFps)));

    auto const timing = limiter.request(Start + 1ms, SixtyFps);

    CHECK(timing.action == RedrawAction::Defer);
    CHECK(timing.delay == interval);
}

TEST_CASE("FrameRateLimiter.resetForgetsTheFrameDrawnLast", "[display][redraw][frame-rate]")
{
    // Every session rebind starts over: the new owner's first frame is not the old one's to pace.
    auto limiter = FrameRateLimiter {};
    REQUIRE(drawsNow(limiter.request(Start, SixtyFps)));
    REQUIRE(limiter.request(Start + 1ms, SixtyFps).action == RedrawAction::Defer);

    limiter.reset();

    CHECK(drawsNow(limiter.request(Start + 1ms, SixtyFps)));
}

TEST_CASE("FrameRateLimiter.minFrameIntervalSpellsTheBudget", "[display][redraw][frame-rate]")
{
    CHECK(minFrameInterval(0) == std::chrono::steady_clock::duration::zero());
    CHECK(minFrameInterval(-10) == std::chrono::steady_clock::duration::zero());
    CHECK(minFrameInterval(1000) == std::chrono::milliseconds { 1 });
    CHECK(minFrameInterval(SixtyFps) == std::chrono::nanoseconds { 16'666'666 });
    CHECK(minFrameInterval(1) == std::chrono::seconds { 1 });
}
