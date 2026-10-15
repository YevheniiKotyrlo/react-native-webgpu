package com.webgpu;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.PixelFormat;
import android.graphics.Region;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import androidx.annotation.NonNull;

import java.util.concurrent.atomic.AtomicLong;

@SuppressLint("ViewConstructor")
public class WebGPUSurfaceView extends SurfaceView implements SurfaceHolder.Callback2 {

  // Unique across views: a first frame reported for a surface the canvas has
  // since replaced, in this view or the one before it, matches no live token.
  private static final AtomicLong sNextSurfaceToken = new AtomicLong();

  WebGPUAPI mApi;
  private boolean mRetired = false;
  private final FirstFrameDrawGate mDrawGate = new FirstFrameDrawGate(new FirstFrameDrawGate.Host() {
    @Override
    public void postDelayed(Runnable action, long delayMillis) {
      WebGPUSurfaceView.this.postDelayed(action, delayMillis);
    }

    @Override
    public void removeCallbacks(Runnable action) {
      WebGPUSurfaceView.this.removeCallbacks(action);
    }
  });

  public WebGPUSurfaceView(Context context, WebGPUAPI api, boolean zOrderOnTop, boolean opaque) {
    super(context);
    mApi = api;
    setZOrderOnTop(zOrderOnTop);
    setOpaque(opaque);
    getHolder().addCallback(this);
  }

  // The format drives the compositor's opaque flag for this layer. It can
  // change on a live surface: SurfaceView reports it through surfaceChanged.
  public void setOpaque(boolean opaque) {
    getHolder().setFormat(opaque ? PixelFormat.OPAQUE : PixelFormat.TRANSLUCENT);
  }

  // The canvas moved to another view while this one stays attached and
  // undrawn (BackingViewKind.retiresWhenReplaced): its surface leaves the
  // renderer now, its draw reports are settled, and the view reports nothing
  // more and punches no hole in the window.
  void retire() {
    releaseSurface();
    mApi = WebGPUAPI.DETACHED;
    mRetired = true;
  }

  @Override
  public boolean gatherTransparentRegion(Region region) {
    return mRetired || super.gatherTransparentRegion(region);
  }

  @Override
  protected void onDetachedFromWindow() {
    mDrawGate.onDetachedFromWindow();
    super.onDetachedFromWindow();
    // surfaceDestroyed() normally fires during detach as well; going offscreen
    // is idempotent, so this is just a safety net for paths where it does not.
    releaseSurface();
  }

  @Override
  public void surfaceCreated(@NonNull SurfaceHolder holder) {
    long surfaceToken = sNextSurfaceToken.incrementAndGet();
    mDrawGate.onSurfaceCreated(surfaceToken);
    mApi.surfaceCreated(holder.getSurface(), surfaceToken, false);
  }

  @Override
  public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
    mApi.surfaceChanged(holder.getSurface());
  }

  @Override
  public void surfaceRedrawNeeded(@NonNull SurfaceHolder holder) {
    // Answered in surfaceRedrawNeededAsync, which SurfaceView calls instead.
  }

  // The draw is reported finished once a frame is in the surface: see
  // FirstFrameDrawGate. GLSurfaceView holds this runnable the same way.
  @Override
  public void surfaceRedrawNeededAsync(@NonNull SurfaceHolder holder, @NonNull Runnable drawingFinished) {
    mDrawGate.onRedrawNeeded(drawingFinished);
  }

  @Override
  public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
    releaseSurface();
  }

  /** Whether surfaceToken names the surface this view has now. Main thread. */
  boolean hasSurface(long surfaceToken) {
    return mDrawGate.hasSurface(surfaceToken);
  }

  /** A frame was presented into the surface surfaceToken names. Main thread. */
  void onFirstFramePresented(long surfaceToken) {
    mDrawGate.onFirstFramePresented(surfaceToken);
  }

  private void releaseSurface() {
    mDrawGate.onSurfaceReleased();
    mApi.surfaceOffscreen();
  }
}
