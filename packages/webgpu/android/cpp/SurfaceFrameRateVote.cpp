#include "SurfaceFrameRateVote.h"

#include <android/choreographer.h>
#include <dlfcn.h>
#include <time.h>

#include <memory>
#include <mutex>

namespace rnwgpu {

namespace {

using SetFrameRateFn = int32_t (*)(ANativeWindow *, float, int8_t);
using PostFrameCallbackDelayed64Fn = void (*)(AChoreographer *,
                                              AChoreographer_frameCallback64,
                                              void *, uint32_t);

// Both functions are newer than the package's minimum SDK, so they are
// resolved at runtime: the vote needs API 30, the 64-bit frame callback API 29.
struct FrameRateApi {
  SetFrameRateFn setFrameRate = nullptr;
  PostFrameCallbackDelayed64Fn postFrameCallbackDelayed64 = nullptr;
};

const FrameRateApi &frameRateApi() {
  static const FrameRateApi api = [] {
    FrameRateApi resolved;
    if (void *nativeWindow = dlopen("libnativewindow.so", RTLD_NOW)) {
      resolved.setFrameRate = reinterpret_cast<SetFrameRateFn>(
          dlsym(nativeWindow, "ANativeWindow_setFrameRate"));
    }
    if (void *android = dlopen("libandroid.so", RTLD_NOW)) {
      resolved.postFrameCallbackDelayed64 =
          reinterpret_cast<PostFrameCallbackDelayed64Fn>(
              dlsym(android, "AChoreographer_postFrameCallbackDelayed64"));
    }
    return resolved;
  }();
  return api;
}

// SurfaceFlinger stops treating a layer as active once it has not presented
// for this long (MAX_ACTIVE_LAYER_PERIOD_NS in Scheduler/LayerInfo.h).
constexpr int64_t kInactiveAfterNanos = 1'200'000'000;

int64_t nowNanos() {
  timespec time{};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

} // namespace

// Frames are presented on the rendering thread while the idle check runs on
// the UI thread's choreographer, which accepts callbacks from any thread.
struct SurfaceFrameRateVote::State {
  std::mutex mutex;
  ANativeWindow *window = nullptr;
  AChoreographer *choreographer = nullptr;
  float maxRefreshRate = 0;
  bool voting = false;
  bool idleCheckPending = false;
  int64_t lastPresentNanos = 0;

  // The caller holds mutex.
  void withdraw() {
    if (voting) {
      frameRateApi().setFrameRate(
          window, 0, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
      voting = false;
    }
  }

  // The caller holds mutex. A frame callback cannot be cancelled, so it holds
  // the state weakly.
  static void scheduleIdleCheck(const std::shared_ptr<State> &state,
                                int64_t delayNanos) {
    state->idleCheckPending = true;
    frameRateApi().postFrameCallbackDelayed64(
        state->choreographer, checkIdle, new std::weak_ptr<State>(state),
        static_cast<uint32_t>((delayNanos + 999'999) / 1'000'000));
  }

  static void checkIdle(int64_t /* frameTimeNanos */, void *data) {
    std::unique_ptr<std::weak_ptr<State>> owner(
        static_cast<std::weak_ptr<State> *>(data));
    auto state = owner->lock();
    if (state == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->idleCheckPending = false;
    if (!state->voting) {
      return;
    }
    const int64_t idleNanos = nowNanos() - state->lastPresentNanos;
    if (idleNanos >= kInactiveAfterNanos) {
      state->withdraw();
      return;
    }
    scheduleIdleCheck(state, kInactiveAfterNanos - idleNanos);
  }
};

SurfaceFrameRateVote::SurfaceFrameRateVote()
    : _state(std::make_shared<State>()) {}

SurfaceFrameRateVote::~SurfaceFrameRateVote() { detach(); }

void SurfaceFrameRateVote::attach(ANativeWindow *window, float maxRefreshRate) {
  detach();
  // Without a looper on this thread the vote could never be withdrawn.
  AChoreographer *choreographer = AChoreographer_getInstance();
  if (window == nullptr || maxRefreshRate <= 0 || choreographer == nullptr) {
    return;
  }
  ANativeWindow_acquire(window);
  std::lock_guard<std::mutex> lock(_state->mutex);
  _state->window = window;
  _state->choreographer = choreographer;
  _state->maxRefreshRate = maxRefreshRate;
}

void SurfaceFrameRateVote::onFramePresented() {
  const FrameRateApi &api = frameRateApi();
  if (api.setFrameRate == nullptr ||
      api.postFrameCallbackDelayed64 == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(_state->mutex);
  if (_state->window == nullptr) {
    return;
  }
  _state->lastPresentNanos = nowNanos();
  if (!_state->voting) {
    api.setFrameRate(_state->window, _state->maxRefreshRate,
                     ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    _state->voting = true;
  }
  if (!_state->idleCheckPending) {
    State::scheduleIdleCheck(_state, kInactiveAfterNanos);
  }
}

void SurfaceFrameRateVote::detach() {
  std::lock_guard<std::mutex> lock(_state->mutex);
  if (_state->window == nullptr) {
    return;
  }
  _state->withdraw();
  ANativeWindow_release(_state->window);
  _state->window = nullptr;
}

} // namespace rnwgpu
