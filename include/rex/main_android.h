#pragma once
/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation SDK
 ******************************************************************************
 * Android runtime glue declarations. The definitions live in
 * src/core/android_runtime.cpp (rexcore, guarded by REX_PLATFORM_ANDROID).
 *
 * The app's JNI glue (SDL activity side) calls SetAndroidApplicationContext()
 * before any runtime subsystem starts.
 ******************************************************************************
 * @added      deivid22srk, 2026 - Android port support
 */

#include <cstdint>
#include <filesystem>
#include <string>

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

namespace rex {

// Device API level (ro.build.version.sdk), cached on first call. At least the
// compile-time minimum (__ANDROID_API__) is returned.
uint32_t GetAndroidApiLevel();

// Provided by the app's JNI glue before runtime startup:
//  - java_vm:         JavaVM* (used to attach native threads for JNI calls)
//  - android_context: jobject of the foreground Activity (global ref taken)
//  - native_library_dir: ApplicationInfo.nativeLibraryDir, where the packaged
//    .so files live (used by GetExecutableFolder() and the GPU plugin loader)
void SetAndroidApplicationContext(void* java_vm, void* android_context,
                                  const char* native_library_dir);

// nativeLibraryDir backing GetExecutableFolder() on Android.
const std::filesystem::path& GetAndroidNativeLibraryDir();

// JNI bridge invoked by rex::filesystem::OpenAndroidContentFileDescriptor.
// Implemented on top of a static Java helper on the activity class that
// returns a ParcelFileDescriptor for the given content URI.
int OpenContentUriFdJavaBridge(const std::string_view uri, const char* mode);

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
