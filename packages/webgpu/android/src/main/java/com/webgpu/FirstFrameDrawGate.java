package com.webgpu;

import java.util.ArrayList;

/**
 * Holds a SurfaceView's draw reports until a frame is presented into its
 * surface. The window punches the hole a surface behind it shows through once
 * the draw is reported finished, so a report sent before the first frame
 * opens the hole on an empty surface, which shows black. A first frame that
 * does not come within FIRST_FRAME_WAIT_MILLIS is not waited for: the
 * reports go out and the hole opens without it. Main thread.
 *
 * Frames are presented on the JS thread, the UI thread or a worklet runtime,
 * so the report of a first frame reaches the main thread posted, possibly
 * after its surface was replaced: each surface carries a token, and a frame
 * reported for another token is not this surface's.
 */
final class FirstFrameDrawGate {
  /**
   * How long a window waits for the first frame: the bound Android 14 puts
   * on a SurfaceSyncGroup the window waits for (TRANSACTION_READY_TIMEOUT).
   * A present that fails, or a canvas that never presents, would otherwise
   * hold the window's draw report for good.
   */
  static final long FIRST_FRAME_WAIT_MILLIS = 1000;

  /** What the gate asks of the view that owns it. */
  interface Host {
    /** Runs action on the main thread after delayMillis, unless removed. */
    void postDelayed(Runnable action, long delayMillis);

    /** Removes an action posted with postDelayed. */
    void removeCallbacks(Runnable action);
  }

  private final Host mHost;
  private final Runnable mFirstFrameDeadline = this::onFirstFrameDeadline;
  private long mSurfaceToken = 0;
  private boolean mFramePresented = false;
  private boolean mDeadlinePosted = false;
  private final ArrayList<Runnable> mPendingDrawsFinished = new ArrayList<>();

  FirstFrameDrawGate(Host host) {
    mHost = host;
  }

  /** A new surface, named by surfaceToken: nothing has been presented into it yet. */
  void onSurfaceCreated(long surfaceToken) {
    mSurfaceToken = surfaceToken;
    mFramePresented = false;
  }

  /** The window asks to be told once the surface has drawn. */
  void onRedrawNeeded(Runnable drawingFinished) {
    if (mFramePresented) {
      drawingFinished.run();
      return;
    }
    mPendingDrawsFinished.add(drawingFinished);
    if (!mDeadlinePosted) {
      mDeadlinePosted = true;
      mHost.postDelayed(mFirstFrameDeadline, FIRST_FRAME_WAIT_MILLIS);
    }
  }

  /** Whether surfaceToken names the surface the view has now. */
  boolean hasSurface(long surfaceToken) {
    return surfaceToken != 0 && surfaceToken == mSurfaceToken;
  }

  /** A frame was presented into the surface surfaceToken names. */
  void onFirstFramePresented(long surfaceToken) {
    if (surfaceToken != mSurfaceToken) {
      return;
    }
    mFramePresented = true;
    cancelDeadline();
    reportPendingDraws();
  }

  /** The surface is gone, so it has nothing left to draw. */
  void onSurfaceReleased() {
    mSurfaceToken = 0;
    mFramePresented = false;
    cancelDeadline();
    reportPendingDraws();
  }

  /** The view left its window, which settles the draws it still waits for. */
  void onDetachedFromWindow() {
    cancelDeadline();
    mPendingDrawsFinished.clear();
  }

  private void onFirstFrameDeadline() {
    mDeadlinePosted = false;
    reportPendingDraws();
  }

  private void cancelDeadline() {
    if (mDeadlinePosted) {
      mDeadlinePosted = false;
      mHost.removeCallbacks(mFirstFrameDeadline);
    }
  }

  private void reportPendingDraws() {
    if (mPendingDrawsFinished.isEmpty()) {
      return;
    }
    ArrayList<Runnable> pending = new ArrayList<>(mPendingDrawsFinished);
    mPendingDrawsFinished.clear();
    for (Runnable drawingFinished : pending) {
      drawingFinished.run();
    }
  }
}
