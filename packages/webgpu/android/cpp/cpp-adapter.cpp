#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <fbjni/fbjni.h>
#include <jni.h>
#include <jsi/jsi.h>

#include <ReactCommon/CallInvokerHolder.h>
#include <android/bitmap.h>
#include <android/hardware_buffer_jni.h>
#include <android/native_window_jni.h>
#include <webgpu/webgpu_cpp.h>

#include "AndroidPlatformContext.h"
#include "GPUCanvasContext.h"
#include "RNWebGPUManager.h"
#include "SurfaceFrameRateVote.h"
#include "WebGPURuntime.h"

#define LOG_TAG "WebGPUModule"

std::shared_ptr<rnwgpu::RNWebGPUManager> manager;

extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUModule_initializeNative(
    JNIEnv *env, jobject thiz, jlong jsRuntime, jobject jsCallInvokerHolder,
    jobject blobModule) {
  auto runtime = reinterpret_cast<facebook::jsi::Runtime *>(jsRuntime);
  jobject globalBlobModule = env->NewGlobalRef(blobModule);
  jobject globalModule = env->NewGlobalRef(thiz);
  auto jsCallInvoker{
      facebook::jni::alias_ref<facebook::react::CallInvokerHolder::javaobject>{
          reinterpret_cast<facebook::react::CallInvokerHolder::javaobject>(
              jsCallInvokerHolder)} -> cthis()->getCallInvoker()};
  auto platformContext = std::make_shared<rnwgpu::AndroidPlatformContext>(
      globalBlobModule, globalModule);
  manager = std::make_shared<rnwgpu::RNWebGPUManager>(runtime, jsCallInvoker,
                                                      platformContext);
}

// Completion of WebGPUModule.snapshotView (UI thread). Reclaims the callbacks
// handed to Java and delivers either the bitmap's pixels or the error.
extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUModule_onViewSnapshot(
    JNIEnv *env, jobject /* this */, jlong callbackPointer, jobject bitmap,
    jstring error) {
  std::unique_ptr<rnwgpu::ViewSnapshotCallbacks> callbacks(
      reinterpret_cast<rnwgpu::ViewSnapshotCallbacks *>(callbackPointer));
  if (!callbacks) {
    return;
  }
  if (error != nullptr) {
    const char *chars = env->GetStringUTFChars(error, nullptr);
    std::string message(chars ? chars : "drawElementImageToTexture failed");
    if (chars) {
      env->ReleaseStringUTFChars(error, chars);
    }
    callbacks->onError(std::move(message));
    return;
  }
  if (bitmap == nullptr) {
    callbacks->onError("drawElementImageToTexture: no bitmap was produced");
    return;
  }
  AndroidBitmapInfo info;
  if (AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
    callbacks->onError(
        "drawElementImageToTexture: unexpected snapshot bitmap format");
    return;
  }
  void *pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      pixels == nullptr) {
    callbacks->onError(
        "drawElementImageToTexture: couldn't lock the snapshot bitmap");
    return;
  }
  rnwgpu::ImageData image;
  image.width = info.width;
  image.height = info.height;
  image.format = wgpu::TextureFormat::RGBA8Unorm;
  // Bitmap.Config.ARGB_8888 stores premultiplied RGBA bytes.
  image.premultiplied = true;
  const size_t rowBytes = static_cast<size_t>(info.width) * 4;
  image.data.resize(rowBytes * info.height);
  const auto *src = static_cast<const uint8_t *>(pixels);
  for (uint32_t row = 0; row < info.height; ++row) {
    std::memcpy(image.data.data() + row * rowBytes, src + row * info.stride,
                rowBytes);
  }
  AndroidBitmap_unlockPixels(env, bitmap);
  callbacks->onPixels(std::move(image));
}

// Completion of WebGPUModule.snapshotView through a hardware buffer (UI
// thread, Android 13 and above): the buffer HardwareRenderer drew the view
// into, and the fence that signals when the drawing is done, as a sync fd
// this function takes ownership of. The GPU draws the buffer into the
// destination itself (GPUQueue::drawElementImageToTexture); nothing is read
// back through the CPU.
extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUModule_onViewSnapshotBuffer(JNIEnv *env,
                                                  jobject /* this */,
                                                  jlong callbackPointer,
                                                  jobject hardwareBuffer,
                                                  jint fenceFd) {
  std::unique_ptr<rnwgpu::ViewSnapshotCallbacks> callbacks(
      reinterpret_cast<rnwgpu::ViewSnapshotCallbacks *>(callbackPointer));
  AHardwareBuffer *buffer =
      hardwareBuffer != nullptr
          ? AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer)
          : nullptr;
  if (!callbacks || buffer == nullptr) {
    if (fenceFd >= 0) {
      ::close(fenceFd);
    }
    if (callbacks) {
      callbacks->onError(
          "drawElementImageToTexture: the renderer produced no buffer");
    }
    return;
  }
  AHardwareBuffer_Desc desc{};
  AHardwareBuffer_describe(buffer, &desc);
  // Java closes its Image and HardwareBuffer as soon as this returns; this
  // reference keeps the memory alive until the frame is released.
  AHardwareBuffer_acquire(buffer);

  rnwgpu::VideoFrameHandle frame;
  frame.handle = buffer;
  frame.width = desc.width;
  frame.height = desc.height;
  // An RGBA_8888 buffer: sampled as a plain color texture.
  frame.pixelFormat = rnwgpu::VideoPixelFormat::BGRA8;
  frame.acquireFenceFd = fenceFd;
  frame.deleter = [buffer, fenceFd]() {
    AHardwareBuffer_release(buffer);
    if (fenceFd >= 0) {
      ::close(fenceFd);
    }
  };
  callbacks->onFrame(std::move(frame));
}

extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUView_onSurfaceChanged(
    JNIEnv *env, jobject thiz, jobject surface, jint contextId, jfloat width,
    jfloat height) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  if (auto info = registry.getSurfaceInfo(contextId)) {
    info->resize(static_cast<int>(width), static_cast<int>(height));
  }
}

namespace {

// Reports a SurfaceView's first presented frame back to its WebGPUView, once,
// from the thread that presented it: the view holds the window's draw report
// until then (FirstFrameDrawGate). The view is held weakly: the surface, and
// this with it, can outlive it.
class FirstFramePresentedNotifier {
public:
  FirstFramePresentedNotifier(JNIEnv *env, jobject view, jlong firstFrameToken)
      : _view(env->NewWeakGlobalRef(view)), _firstFrameToken(firstFrameToken) {
    jclass cls = env->GetObjectClass(view);
    _onFirstFramePresented =
        env->GetMethodID(cls, "onFirstFramePresented", "(J)V");
    env->DeleteLocalRef(cls);
  }

  ~FirstFramePresentedNotifier() {
    // May run on any thread the surface is released from.
    facebook::jni::ThreadScope scope;
    JNIEnv *env = facebook::jni::Environment::current();
    if (env != nullptr && _view != nullptr) {
      env->DeleteWeakGlobalRef(_view);
    }
  }

  void onFramePresented() {
    if (_reported.exchange(true)) {
      return;
    }
    facebook::jni::ThreadScope scope;
    JNIEnv *env = facebook::jni::Environment::current();
    if (env == nullptr || _onFirstFramePresented == nullptr) {
      return;
    }
    jobject local = env->NewLocalRef(_view);
    if (local == nullptr) {
      return; // the view was garbage collected
    }
    env->CallVoidMethod(local, _onFirstFramePresented, _firstFrameToken);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(local);
  }

private:
  jweak _view;
  jmethodID _onFirstFramePresented = nullptr;
  jlong _firstFrameToken;
  std::atomic<bool> _reported{false};
};

// A no-argument void method of a view, called from native through a weak
// reference, so that a view RN has dropped is not kept alive by a
// SurfaceInfo that outlives it (the registry keeps the SurfaceInfo until
// onDropViewInstance).
struct WeakViewMethod {
  jweak view = nullptr;
  jmethodID method = nullptr;

  WeakViewMethod(JNIEnv *env, jobject thiz, const char *name) {
    view = env->NewWeakGlobalRef(thiz);
    jclass cls = env->GetObjectClass(thiz);
    method = env->GetMethodID(cls, name, "()V");
    env->DeleteLocalRef(cls);
  }

  ~WeakViewMethod() {
    // May run on any thread; ThreadScope attaches if needed.
    facebook::jni::ThreadScope scope;
    JNIEnv *env = facebook::jni::Environment::current();
    if (env != nullptr && view != nullptr) {
      env->DeleteWeakGlobalRef(view);
    }
  }

  // Returns whether the view was there to take the call. Called outside
  // SurfaceInfo's locks.
  bool operator()() const {
    facebook::jni::ThreadScope scope;
    JNIEnv *env = facebook::jni::Environment::current();
    if (env == nullptr || method == nullptr) {
      return false;
    }
    jobject local = env->NewLocalRef(view);
    if (local == nullptr) {
      return false; // the view was garbage collected
    }
    env->CallVoidMethod(local, method);
    const bool called = !env->ExceptionCheck();
    if (!called) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(local);
    return called;
  }
};

} // namespace

extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUView_onSurfaceCreate(
    JNIEnv *env, jobject thiz, jobject jSurface, jint contextId, jfloat width,
    jfloat height, jfloat maxRefreshRate, jlong firstFrameToken,
    jboolean presentsBehindPendingDraw, jboolean adoptsBeforeFirstDraw) {
  if (manager == nullptr) {
    return;
  }
  // ANativeWindow_fromSurface acquires a reference; SurfaceInfo releases it
  // (via the releaser below) once it is done with the window.
  auto window = ANativeWindow_fromSurface(env, jSurface);
  if (window == nullptr) {
    return;
  }
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto &runtime = rnwgpu::WebGPURuntime::shared();
  auto gpu = runtime.instance();
  auto surface = runtime.makeSurface(window);
  // Only a SurfaceView passes a refresh rate: it is a layer of its own, while a
  // TextureView draws into its window's layer.
  std::shared_ptr<rnwgpu::SurfaceFrameRateVote> frameRateVote;
  if (maxRefreshRate > 0) {
    frameRateVote = std::make_shared<rnwgpu::SurfaceFrameRateVote>();
    frameRateVote->attach(window, maxRefreshRate);
  }
  // Only a SurfaceView behind its window waits for its first frame.
  std::shared_ptr<FirstFramePresentedNotifier> firstFrameNotifier;
  if (firstFrameToken != 0) {
    firstFrameNotifier = std::make_shared<FirstFramePresentedNotifier>(
        env, thiz, firstFrameToken);
  }
  rnwgpu::NativeSurfaceDidPresent didPresent;
  if (frameRateVote || firstFrameNotifier) {
    didPresent = [frameRateVote, firstFrameNotifier]() {
      if (frameRateVote) {
        frameRateVote->onFramePresented();
      }
      if (firstFrameNotifier) {
        firstFrameNotifier->onFramePresented();
      }
    };
  }
  rnwgpu::NativeSurfacePresentPoster presentPoster;
  if (presentsBehindPendingDraw) {
    auto postPresent = std::make_shared<WeakViewMethod>(
        env, thiz, "postPresentBehindPendingDraw");
    presentPoster = [postPresent]() { return (*postPresent)(); };
  }
  // Find-or-create + attach runs atomically under the registry lock so a
  // concurrent destroyContext cannot orphan this surface.
  auto info = registry.attachSurface(
      contextId, gpu, static_cast<int>(width), static_cast<int>(height), window,
      surface,
      [](void *nativeSurface) {
        ANativeWindow_release(static_cast<ANativeWindow *>(nativeSurface));
      },
      std::move(didPresent), std::move(presentPoster));
  // A TextureView given its surface before its first draw shows the latest
  // frame in that draw only if it is presented into the surface first.
  if (adoptsBeforeFirstDraw) {
    info->applyPendingAttach();
  }
  // The attach is adopted at the next frame boundary by the rendering thread;
  // schedule a flush so contexts that are not currently rendering still pick
  // it up (and present their last offscreen frame).
  manager->flushPendingSurfaceTransition(info);
}

// The present WebGPUView.postPresentBehindPendingDraw posted (UI thread).
extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUView_presentPostedFrame(
    JNIEnv *env, jobject thiz, jint contextId) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  if (auto info = registry.getSurfaceInfo(contextId)) {
    info->presentPostedFrame();
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUView_switchToOffscreenSurface(JNIEnv *env, jobject thiz,
                                                    jint contextId) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  if (auto info = registry.getSurfaceInfo(contextId)) {
    info->switchToOffscreen();
  }
}

extern "C" JNIEXPORT void JNICALL Java_com_webgpu_WebGPUView_onViewDestroyed(
    JNIEnv *env, jobject thiz, jint contextId) {
  // The view dies with its Canvas (contextIds are never reused), so view
  // teardown retires the registry entry. The JS-side cleanup
  // (RNWebGPU.destroyContext) only handles entries that never had a native
  // surface; see RNWebGPU::destroyContext for the ownership split.
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  if (auto info = registry.getSurfaceInfo(contextId)) {
    info->detachSurface();
  }
  registry.removeSurfaceInfo(contextId);
}

// --- WebGPUHardwareBufferView (AHB-pool presentation)
// ---------------------------------

// Turn on pool mode for this context. dpW/dpH is the canvas-client (dp) size;
// the native pool buffers are sized from the canvas drawing buffer lazily.
extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nEnablePool(JNIEnv *env, jobject thiz,
                                                     jint contextId, jint dpW,
                                                     jint dpH) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfoOrCreate(
      contextId, rnwgpu::WebGPURuntime::shared().instance(),
      static_cast<int>(dpW), static_cast<int>(dpH));
  // The native fence waiter wakes the UI thread through it when a frame is
  // ready.
  auto waker =
      std::make_shared<WeakViewMethod>(env, thiz, "onNativeFrameReady");
  info->setPoolFrameReadyCallback([waker]() { (*waker)(); });
  info->enablePool(static_cast<int>(dpW), static_cast<int>(dpH));
  // The latest frame is published into the pool at the next frame boundary;
  // schedule a flush so a context that is not rendering still shows it.
  if (manager != nullptr) {
    manager->flushPendingSurfaceTransition(info);
  }
}

// Keep the canvas-client (dp) size in sync on resize.
extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nSetClientSize(JNIEnv *env,
                                                        jobject thiz,
                                                        jint contextId,
                                                        jint dpW, jint dpH) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfo(contextId);
  if (info != nullptr) {
    info->setPoolClientSize(static_cast<int>(dpW), static_cast<int>(dpH));
  }
}

// The HardwareBuffer backing a (generation, slot), for the view to wrap in a
// Bitmap. Returns null for a retired generation.
extern "C" JNIEXPORT jobject JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nGetHardwareBuffer(
    JNIEnv *env, jobject thiz, jint contextId, jint generation, jint slot) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfo(contextId);
  if (info == nullptr) {
    return nullptr;
  }
  void *ahb = info->poolBufferForDisplay(static_cast<uint32_t>(generation),
                                         static_cast<int>(slot));
  if (ahb == nullptr) {
    return nullptr;
  }
  // toHardwareBuffer acquires its own ref for the returned jobject; the pool
  // keeps the underlying buffer alive independently.
  return AHardwareBuffer_toHardwareBuffer(env,
                                          static_cast<AHardwareBuffer *>(ahb));
}

// Latest signaled frame ready to display, encoded (generation << 32 | slot), or
// -1 when nothing is new. Called on the UI thread after onNativeFrameReady.
extern "C" JNIEXPORT jlong JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nPollReady(JNIEnv *env, jobject thiz,
                                                    jint contextId) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfo(contextId);
  if (info == nullptr) {
    return -1;
  }
  return info->poolPollReady();
}

// The view is done displaying (and holding) a slot; return it to the pool.
extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nReleaseSlot(JNIEnv *env, jobject thiz,
                                                      jint contextId,
                                                      jint generation,
                                                      jint slot) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfo(contextId);
  if (info != nullptr) {
    info->poolReleaseSlot(static_cast<uint32_t>(generation),
                          static_cast<int>(slot));
  }
}

// View detached / hidden: keep the canvas alive by falling back to an offscreen
// texture (mirrors switchToOffscreenSurface for the surface path).
extern "C" JNIEXPORT void JNICALL
Java_com_webgpu_WebGPUHardwareBufferView_nSwitchToOffscreen(JNIEnv *env,
                                                            jobject thiz,
                                                            jint contextId) {
  auto &registry = rnwgpu::SurfaceRegistry::getInstance();
  auto info = registry.getSurfaceInfo(contextId);
  if (info != nullptr) {
    info->setPoolFrameReadyCallback(nullptr);
    info->switchToOffscreen();
  }
}
