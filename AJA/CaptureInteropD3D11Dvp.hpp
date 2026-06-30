#pragma once
#include <AJA/AjaDmaLockPolicy.hpp>
#include <gpudirect/DvpCaptureD3D11.hpp>

namespace Gfx::AJA
{

/**
 * @brief AJA D3D11 capture via NVIDIA DVP.
 *
 * Thin instantiation of the shared Gfx::gpudirect::DvpCaptureD3D11 with AJA's
 * page-lock policy and pixel-format -> DVP-format rule (ARGB -> BGRA8, else
 * RGBA8). Behaviour-equivalent to the previous standalone shim.
 */
struct CaptureInteropD3D11Dvp final
    : Gfx::gpudirect::DvpCaptureD3D11<AjaDmaLockPolicy>
{
  CaptureInteropD3D11Dvp(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : Gfx::gpudirect::DvpCaptureD3D11<AjaDmaLockPolicy>{
            AjaDmaLockPolicy{card},
            (pixfmt == AJAInputPixelFormat::ARGB) ? NV_DVP_FORMAT_BGRA8
                                                  : NV_DVP_FORMAT_RGBA8,
            "DVP-D3D11"}
  {
  }
};

} // namespace Gfx::AJA
