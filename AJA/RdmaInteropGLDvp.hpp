#pragma once
#include <AJA/AjaDmaLockPolicy.hpp>
#include <AJA/AjaDvpEncoder.hpp>
#include <gpudirect/DvpOutputGl.hpp>

#include <ntv2enums.h>

namespace Gfx::AJA
{

/**
 * @brief AJA OpenGL output via NVIDIA DVP.
 *
 * Thin instantiation of the shared Gfx::gpudirect::DvpOutputGl with AJA's
 * page-lock policy and the AJA frame-buffer-format encoder closures
 * (UYVY/V210/BGRA + their m_outTexture). Behaviour-equivalent to the previous
 * standalone shim.
 */
struct RdmaInteropGLDvp final : Gfx::gpudirect::DvpOutputGl<AjaDmaLockPolicy>
{
  RdmaInteropGLDvp(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : Gfx::gpudirect::DvpOutputGl<AjaDmaLockPolicy>{
            AjaDmaLockPolicy{card},
            [fmt] { return ajaMakeFragmentEncoder(fmt); },
            [fmt](score::gfx::GPUVideoEncoder* e) {
              return ajaEncoderOutputTexture(e, fmt);
            },
            "DVP-GL"}
  {
  }
};

} // namespace Gfx::AJA
