#pragma once
#include <AJA/AjaDmaLockPolicy.hpp>
#include <gpudirect/DvpCaptureGl.hpp>

namespace Gfx::AJA
{

/**
 * @brief AJA OpenGL capture via NVIDIA DVP.
 *
 * Thin instantiation of the shared Gfx::gpudirect::DvpCaptureGl with AJA's
 * page-lock policy (CNTV2Card::DMABufferLock) and the AJA pixel-format ->
 * DVP-format rule (ARGB -> BGRA8, else RGBA8). All the DVP/GL machinery lives
 * in the template; this preserves the previous standalone shim's behaviour.
 */
struct CaptureInteropGLDvp final
    : Gfx::gpudirect::DvpCaptureGl<AjaDmaLockPolicy>
{
  CaptureInteropGLDvp(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : Gfx::gpudirect::DvpCaptureGl<AjaDmaLockPolicy>{
            AjaDmaLockPolicy{card},
            (pixfmt == AJAInputPixelFormat::ARGB) ? NV_DVP_FORMAT_BGRA8
                                                  : NV_DVP_FORMAT_RGBA8,
            "DVP-GL"}
  {
  }
};

} // namespace Gfx::AJA
