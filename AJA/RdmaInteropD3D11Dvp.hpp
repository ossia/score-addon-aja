#pragma once
#include <AJA/AjaDmaLockPolicy.hpp>
#include <AJA/AjaDvpEncoder.hpp>
#include <gpudirect/DvpOutputD3D11.hpp>

#include <ntv2enums.h>

namespace Gfx::AJA
{

/**
 * @brief AJA D3D11 output via NVIDIA DVP.
 *
 * Thin instantiation of the shared Gfx::gpudirect::DvpOutputD3D11 with AJA's
 * page-lock policy and frame-buffer-format encoder closures. Behaviour-
 * equivalent to the previous standalone shim.
 */
struct RdmaInteropD3D11Dvp final : Gfx::gpudirect::DvpOutputD3D11<AjaDmaLockPolicy>
{
  RdmaInteropD3D11Dvp(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : Gfx::gpudirect::DvpOutputD3D11<AjaDmaLockPolicy>{
            AjaDmaLockPolicy{card},
            [fmt] { return ajaMakeFragmentEncoder(fmt); },
            [fmt](score::gfx::GPUVideoEncoder* e) {
              return ajaEncoderOutputTexture(e, fmt);
            },
            "DVP-D3D11"}
  {
  }
};

} // namespace Gfx::AJA
