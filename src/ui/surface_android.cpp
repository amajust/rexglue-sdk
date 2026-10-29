/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation SDK
 ******************************************************************************
 * Android ANativeWindow presentation surface implementation.
 ******************************************************************************
 * @added      deivid22srk, 2026 - Android port support
 */

#include <rex/ui/surface_android.h>

namespace rex {
namespace ui {

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out,
                                             uint32_t& height_out) const {
  if (!window_) {
    return false;
  }
  // ANativeWindow dimensions are the physical surface pixels (already scaled
  // by the compositor's buffer transform), matching the 1:1 aspect contract
  // documented on Surface::GetSize.
  const int32_t width = ANativeWindow_getWidth(window_);
  const int32_t height = ANativeWindow_getHeight(window_);
  if (width <= 0 || height <= 0) {
    return false;
  }
  width_out = static_cast<uint32_t>(width);
  height_out = static_cast<uint32_t>(height);
  return true;
}

}  // namespace ui
}  // namespace rex
