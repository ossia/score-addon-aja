#pragma once
#include <Gfx/Graph/encoders/BGRA.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>
#include <Gfx/Graph/encoders/V210.hpp>

#include <ntv2enums.h>

#include <memory>

namespace Gfx::AJA
{

/// Fragment encoder for an AJA frame-buffer format (output DVP path).
inline std::unique_ptr<score::gfx::GPUVideoEncoder>
ajaMakeFragmentEncoder(NTV2FrameBufferFormat fmt)
{
  switch(fmt)
  {
    case NTV2_FBF_8BIT_YCBCR:
      return std::make_unique<score::gfx::UYVYEncoder>();
    case NTV2_FBF_10BIT_YCBCR:
      return std::make_unique<score::gfx::V210Encoder>();
    case NTV2_FBF_ARGB:
      return std::make_unique<score::gfx::BGRAEncoder>();
    default:
      return nullptr;
  }
}

/// The fragment encoders expose `m_outTexture` as a public field; rather than
/// add a virtual getter to the encoder base, cast to the concrete type per
/// format.
inline QRhiTexture* ajaEncoderOutputTexture(
    score::gfx::GPUVideoEncoder* enc, NTV2FrameBufferFormat fmt)
{
  if(!enc)
    return nullptr;
  switch(fmt)
  {
    case NTV2_FBF_8BIT_YCBCR:
      return static_cast<score::gfx::UYVYEncoder*>(enc)->m_outTexture;
    case NTV2_FBF_10BIT_YCBCR:
      return static_cast<score::gfx::V210Encoder*>(enc)->m_outTexture;
    case NTV2_FBF_ARGB:
      return static_cast<score::gfx::BGRAEncoder*>(enc)->m_outTexture;
    default:
      return nullptr;
  }
}

} // namespace Gfx::AJA
