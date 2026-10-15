package com.webgpu;

import static org.junit.Assert.assertEquals;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.Test;

public class FirstFrameDrawGateTest {
  private static final long FIRST_SURFACE = 1;
  private static final long SECOND_SURFACE = 2;

  private static final class ManualClock implements FirstFrameDrawGate.Host {
    final List<Runnable> posted = new ArrayList<>();
    final List<Long> delays = new ArrayList<>();

    @Override
    public void postDelayed(Runnable action, long delayMillis) {
      posted.add(action);
      delays.add(delayMillis);
    }

    @Override
    public void removeCallbacks(Runnable action) {
      posted.remove(action);
    }

    void elapse() {
      List<Runnable> due = new ArrayList<>(posted);
      posted.clear();
      for (Runnable action : due) {
        action.run();
      }
    }
  }

  private final List<String> mReported = new ArrayList<>();
  private final ManualClock mClock = new ManualClock();
  private final FirstFrameDrawGate mGate = new FirstFrameDrawGate(mClock);

  private Runnable draw(String name) {
    return () -> mReported.add(name);
  }

  @Test
  public void drawRequestedBeforeTheFirstFrameIsReportedWithIt() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("first"));
    assertEquals(Arrays.asList(), mReported);

    mGate.onFirstFramePresented(FIRST_SURFACE);
    assertEquals(Arrays.asList("first"), mReported);
  }

  @Test
  public void drawRequestedAfterTheFirstFrameIsReportedAtOnce() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onFirstFramePresented(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("resize"));
    assertEquals(Arrays.asList("resize"), mReported);
    assertEquals(0, mClock.posted.size());
  }

  @Test
  public void newSurfaceWaitsForItsOwnFirstFrame() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onFirstFramePresented(FIRST_SURFACE);
    mGate.onSurfaceReleased();
    mGate.onSurfaceCreated(SECOND_SURFACE);
    mGate.onRedrawNeeded(draw("second surface"));
    assertEquals(Arrays.asList(), mReported);

    mGate.onFirstFramePresented(SECOND_SURFACE);
    assertEquals(Arrays.asList("second surface"), mReported);
  }

  @Test
  public void aFrameReportedForAnEarlierSurfaceDoesNotOpenTheNewOne() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onSurfaceReleased();
    mGate.onSurfaceCreated(SECOND_SURFACE);
    mGate.onRedrawNeeded(draw("second surface"));

    mGate.onFirstFramePresented(FIRST_SURFACE);
    assertEquals(Arrays.asList(), mReported);

    mGate.onFirstFramePresented(SECOND_SURFACE);
    assertEquals(Arrays.asList("second surface"), mReported);
  }

  @Test
  public void surfaceReleasedBeforeItsFirstFrameReportsItsPendingDraws() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("a"));
    mGate.onRedrawNeeded(draw("b"));
    mGate.onSurfaceReleased();
    assertEquals(Arrays.asList("a", "b"), mReported);
    assertEquals(0, mClock.posted.size());
  }

  @Test
  public void detachingDropsPendingDrawsWithoutReportingThem() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("dropped"));
    mGate.onDetachedFromWindow();
    mGate.onSurfaceReleased();
    mGate.onFirstFramePresented(FIRST_SURFACE);
    mClock.elapse();
    assertEquals(Arrays.asList(), mReported);
  }

  @Test
  public void eachDrawIsReportedOnce() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("once"));
    mGate.onFirstFramePresented(FIRST_SURFACE);
    mGate.onFirstFramePresented(FIRST_SURFACE);
    mGate.onSurfaceReleased();
    mClock.elapse();
    assertEquals(Arrays.asList("once"), mReported);
  }

  @Test
  public void aFirstFrameThatNeverComesIsWaitedForOnlyUntilTheDeadline() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("held"));
    assertEquals(Arrays.asList(Long.valueOf(FirstFrameDrawGate.FIRST_FRAME_WAIT_MILLIS)), mClock.delays);

    mClock.elapse();
    assertEquals(Arrays.asList("held"), mReported);
  }

  @Test
  public void oneDeadlineCoversEveryDrawWaitingForTheSameFrame() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("a"));
    mGate.onRedrawNeeded(draw("b"));
    assertEquals(1, mClock.posted.size());

    mClock.elapse();
    assertEquals(Arrays.asList("a", "b"), mReported);
  }

  @Test
  public void theFirstFrameCancelsTheDeadline() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("framed"));
    mGate.onFirstFramePresented(FIRST_SURFACE);
    assertEquals(0, mClock.posted.size());
    assertEquals(Arrays.asList("framed"), mReported);
  }

  @Test
  public void detachingCancelsTheDeadline() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("dropped"));
    mGate.onDetachedFromWindow();
    assertEquals(0, mClock.posted.size());
  }

  @Test
  public void aDrawAfterTheDeadlineWaitsForTheFirstFrameAgain() {
    mGate.onSurfaceCreated(FIRST_SURFACE);
    mGate.onRedrawNeeded(draw("first"));
    mClock.elapse();
    mGate.onRedrawNeeded(draw("resize"));
    assertEquals(Arrays.asList("first"), mReported);
    assertEquals(1, mClock.posted.size());

    mGate.onFirstFramePresented(FIRST_SURFACE);
    assertEquals(Arrays.asList("first", "resize"), mReported);
  }
}
