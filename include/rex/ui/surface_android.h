#pragma once
/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation SDK
 ******************************************************************************
 * Android ANativeWindow-backed presentation surface (Vulkan
 * VK_KHR_android_surface). Created by WindowSDL::CreateSurfaceImpl from the
 * SDL window's SDL_PROP_WINDOW_ANDROID_NATIVE_WINDOW_POINTER property.
 ******************************************************************************
 * @added      deivid22srk, 2026 - Android port support
 */

#include <cstdint>

#include <android/native_window.h>

#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
