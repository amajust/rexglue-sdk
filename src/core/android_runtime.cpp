/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation SDK
 ******************************************************************************
 * Android runtime glue: API level query, JNI context plumbing for the app's
 * activity, ContentResolver fd bridge, and the implementations of the
 * AndroidInitialize/Shutdown hooks declared across the SDK headers.
 *
 * The app's JNI glue (SDL activity side) calls
 * rex::SetAndroidApplicationContext() before the runtime starts.
 ******************************************************************************
 * @added      deivid22srk, 2026 - Android port support
 */

#include <rex/main_android.h>

#if REX_PLATFORM_ANDROID

#include <jni.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/system_properties.h>

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system.h>

namespace rex {

namespace {

// Guard everything that touches the JNI side; the context is set exactly once
// from the app's JNI glue before any runtime subsystem starts.
std::mutex g_android_context_mutex;
JavaVM* g_java_vm = nullptr;
jobject g_activity = nullptr;      // global ref
jclass g_activity_class = nullptr; // global ref (activity's own class)
jmethodID g_open_content_fd_mid = nullptr;
jclass g_pfd_class = nullptr; // global ref (android.os.ParcelFileDescriptor)
jmethodID g_detach_fd_mid = nullptr;
std::filesystem::path g_native_library_dir;
std::atomic<bool> g_android_context_ready{false};

JNIEnv* GetThreadEnvOrNull() {
  if (!g_java_vm) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  const jint state = g_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (state == JNI_OK) {
    return env;
  }
  if (state == JNI_EDETACHED) {
    // Native threads spawned by the runtime (std::thread / pthread) attach on
    // demand; bionic cleans the attachment up when the thread exits.
    if (g_java_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      return env;
    }
  }
  return nullptr;
}

void ClearJavaException(JNIEnv* env, const char* where) {
  if (env->ExceptionCheck()) {
    REXLOG_ERROR("android_runtime: Java exception in {}", where);
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

// Resolves the Java helper methods on the activity class. Must run on a thread
// attached to the VM from the app's own classloader context (JNI_OnLoad /
// Activity.onCreate) so app classes are findable.
bool ResolveJavaBridges(JNIEnv* env, jobject activity) {
  jclass activity_class = env->GetObjectClass(activity);
  if (!activity_class) {
    REXLOG_ERROR("android_runtime: GetObjectClass(activity) failed");
    return false;
  }
  g_activity_class = static_cast<jclass>(env->NewGlobalRef(activity_class));
  env->DeleteLocalRef(activity_class);
  if (!g_activity_class) {
    return false;
  }

  g_open_content_fd_mid = env->GetStaticMethodID(
      g_activity_class, "openContentFd",
      "(Ljava/lang/String;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
  if (!g_open_content_fd_mid) {
    ClearJavaException(env, "GetStaticMethodID(openContentFd)");
    return false;
  }

  jclass pfd_class = env->FindClass("android/os/ParcelFileDescriptor");
  if (!pfd_class) {
    ClearJavaException(env, "FindClass(ParcelFileDescriptor)");
    return false;
  }
  g_pfd_class = static_cast<jclass>(env->NewGlobalRef(pfd_class));
  env->DeleteLocalRef(pfd_class);
  if (!g_pfd_class) {
    return false;
  }

  g_detach_fd_mid = env->GetMethodID(g_pfd_class, "detachFd", "()I");
  if (!g_detach_fd_mid) {
    ClearJavaException(env, "GetMethodID(detachFd)");
    return false;
  }
  return true;
}

}  // namespace

uint32_t GetAndroidApiLevel() {
  static const uint32_t api_level = [] {
    char value[16] = {0};
    // __system_property_get is deprecated but stable across every Android
    // release that matters here and avoids pulling in the prop_info callback
    // dance; a wrong read simply falls back to the compile-time minimum.
    const int len = __system_property_get("ro.build.version.sdk", value);
    uint32_t level = 0;
    if (len > 0) {
      level = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    }
    if (level < static_cast<uint32_t>(__ANDROID_API__)) {
      level = static_cast<uint32_t>(__ANDROID_API__);
    }
    return level;
  }();
  return api_level;
}

void SetAndroidApplicationContext(void* java_vm, void* android_context,
                                  const char* native_library_dir) {
  std::lock_guard<std::mutex> lock(g_android_context_mutex);
  assert_not_null(java_vm);
  g_java_vm = static_cast<JavaVM*>(java_vm);

  // The native library dir powers GetExecutableFolder() (GPU plugin staging,
  // asset lookups) and must not depend on JNI state: wiring it before the
  // activity handling keeps a headless / activity-less start able to load
  // librexgpu-*.so from next to libmain.so.
  if (native_library_dir && *native_library_dir) {
    g_native_library_dir = std::filesystem::path(native_library_dir);
  }

  JNIEnv* env = GetThreadEnvOrNull();
  if (!env) {
    REXLOG_ERROR("android_runtime: no JNI env while setting the app context");
    return;
  }

  if (!android_context) {
    // No activity object: the Java bridges stay unavailable (content:// fd
    // opening), but everything filesystem/plugin related above works.
    REXLOG_INFO("android_runtime: no activity context (headless), lib dir {}",
                g_native_library_dir.string());
    return;
  }

  if (g_activity) {
    env->DeleteGlobalRef(g_activity);
    g_activity = nullptr;
  }
  g_activity = env->NewGlobalRef(static_cast<jobject>(android_context));
  if (!g_activity) {
    REXLOG_ERROR("android_runtime: NewGlobalRef(activity) failed");
    return;
  }

  if (!ResolveJavaBridges(env, g_activity)) {
    REXLOG_ERROR("android_runtime: failed to resolve Java bridges");
    return;
  }

  g_android_context_ready.store(true, std::memory_order_release);
  REXLOG_INFO("android_runtime: context ready (api {}, lib dir {})",
              GetAndroidApiLevel(), g_native_library_dir.string());
}

const std::filesystem::path& GetAndroidNativeLibraryDir() {
  return g_native_library_dir;
}

int OpenContentUriFdJavaBridge(const std::string_view uri, const char* mode) {
  JNIEnv* env = GetThreadEnvOrNull();
  if (!env || !g_android_context_ready.load(std::memory_order_acquire)) {
    return -1;
  }

  jstring uri_str = env->NewStringUTF(std::string(uri).c_str());
  if (!uri_str) {
    ClearJavaException(env, "NewStringUTF(uri)");
    return -1;
  }
  jstring mode_str = env->NewStringUTF(mode ? mode : "r");
  if (!mode_str) {
    env->DeleteLocalRef(uri_str);
    ClearJavaException(env, "NewStringUTF(mode)");
    return -1;
  }

  jobject pfd = env->CallStaticObjectMethod(g_activity_class, g_open_content_fd_mid, uri_str,
                                            mode_str);
  env->DeleteLocalRef(uri_str);
  env->DeleteLocalRef(mode_str);
  if (env->ExceptionCheck()) {
    ClearJavaException(env, "openContentFd");
    return -1;
  }
  if (!pfd) {
    return -1;
  }

  const jint fd = env->CallIntMethod(pfd, g_detach_fd_mid);
  env->DeleteLocalRef(pfd);
  if (env->ExceptionCheck()) {
    ClearJavaException(env, "detachFd");
    return -1;
  }
  return static_cast<int>(fd);
}

}  // namespace rex

namespace rex::filesystem {

void AndroidInitialize() {
  // All state is owned by rex::SetAndroidApplicationContext; nothing extra to
  // do here. Kept as a hook for symmetry with the other subsystems.
}

void AndroidShutdown() {
  // JNI-backed state is torn down by rex::ShutdownAndroidSystem(); nothing
  // filesystem-specific to release here.
}

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  return rex::OpenContentUriFdJavaBridge(uri, mode);
}

}  // namespace rex::filesystem

namespace rex {

bool InitializeAndroidSystemForApplicationContext() {
  if (!g_android_context_ready.load(std::memory_order_acquire)) {
    REXLOG_ERROR("android_runtime: app context was never set - call "
                 "rex::SetAndroidApplicationContext() before runtime startup");
    return false;
  }
  return true;
}

void ShutdownAndroidSystem() {
  std::lock_guard<std::mutex> lock(g_android_context_mutex);
  JNIEnv* env = GetThreadEnvOrNull();
  if (env) {
    if (g_activity) {
      env->DeleteGlobalRef(g_activity);
      g_activity = nullptr;
    }
    if (g_activity_class) {
      env->DeleteGlobalRef(g_activity_class);
      g_activity_class = nullptr;
    }
    if (g_pfd_class) {
      env->DeleteGlobalRef(g_pfd_class);
      g_pfd_class = nullptr;
    }
  }
  g_open_content_fd_mid = nullptr;
  g_detach_fd_mid = nullptr;
  g_android_context_ready.store(false, std::memory_order_release);
}

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
