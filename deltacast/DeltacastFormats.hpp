#pragma once
#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#include <QString>

#include <cstdint>

namespace Gfx::Deltacast
{

/// Neutral video-format token ("1080p5994", "2160p60", ...) -> VHD_VIDEOSTANDARD.
/// 59.94/29.97 share the 60/30Hz standard (the clock divisor selects the
/// fractional rate); defaults to 1080p60 for unknown tokens.
inline VHD_VIDEOSTANDARD vhdStandardFromToken(const QString& f) noexcept
{
  if(f == "1080p2398" || f == "1080p24") return VHD_VIDEOSTD_S274M_1080p_24Hz;
  if(f == "1080p25")                     return VHD_VIDEOSTD_S274M_1080p_25Hz;
  if(f == "1080p2997" || f == "1080p30") return VHD_VIDEOSTD_S274M_1080p_30Hz;
  if(f == "1080p50")                     return VHD_VIDEOSTD_S274M_1080p_50Hz;
  if(f == "1080p5994" || f == "1080p60") return VHD_VIDEOSTD_S274M_1080p_60Hz;
  if(f == "1080i50")                     return VHD_VIDEOSTD_S274M_1080i_50Hz;
  if(f == "1080i5994" || f == "1080i60") return VHD_VIDEOSTD_S274M_1080i_60Hz;
  if(f == "720p50")                      return VHD_VIDEOSTD_S296M_720p_50Hz;
  if(f == "720p5994" || f == "720p60")   return VHD_VIDEOSTD_S296M_720p_60Hz;
  if(f == "2160p25")                     return VHD_VIDEOSTD_3840x2160p_25Hz;
  if(f == "2160p2997" || f == "2160p30") return VHD_VIDEOSTD_3840x2160p_30Hz;
  if(f == "2160p50")                     return VHD_VIDEOSTD_3840x2160p_50Hz;
  if(f == "2160p5994" || f == "2160p60") return VHD_VIDEOSTD_3840x2160p_60Hz;
  return VHD_VIDEOSTD_S274M_1080p_60Hz;
}

/// TX interface (SMPTE mapping) for a video standard. RX auto-detects its own.
inline VHD_INTERFACE vhdInterfaceFromStandard(VHD_VIDEOSTANDARD s) noexcept
{
  switch(s)
  {
    case VHD_VIDEOSTD_S274M_1080p_50Hz:
    case VHD_VIDEOSTD_S274M_1080p_60Hz:
      return VHD_INTERFACE_3G_A_425_1;
    case VHD_VIDEOSTD_3840x2160p_25Hz:
    case VHD_VIDEOSTD_3840x2160p_30Hz:
    case VHD_VIDEOSTD_3840x2160p_50Hz:
    case VHD_VIDEOSTD_3840x2160p_60Hz:
      return VHD_INTERFACE_12G_2082_10;
    default:
      return VHD_INTERFACE_HD_292_1; // 720p / 1080i / 1080p<=30
  }
}

/// Pixel-format token -> VHD buffer packing. Defaults to 8-bit YUV 4:2:2.
inline VHD_BUFFERPACKING vhdPackingFromToken(const QString& p) noexcept
{
  if(p == "YCbCr10")              return VHD_BUFPACK_VIDEO_YUV422_10;
  if(p == "RGB8" || p == "BGRA8") return VHD_BUFPACK_VIDEO_RGB_32;
  return VHD_BUFPACK_VIDEO_YUV422_8; // "YCbCr8" + default
}

/// VHD buffer packing -> neutral wire format (for makeWireEncoder/Decoder).
inline score::gfx::interop::VideoPixelFormat
neutralFromPacking(VHD_BUFFERPACKING pack) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;
  switch(pack)
  {
    case VHD_BUFPACK_VIDEO_YUV422_8:  return F::UYVY422;
    case VHD_BUFPACK_VIDEO_YUV422_10: return F::V210;
    case VHD_BUFPACK_VIDEO_RGB_32:    return F::RGBA8; // byte order TBD on HW
    default:                          return F::UYVY422;
  }
}

/// Bytes per scanline for a packing at @p width (matches VHD's raster rules).
inline std::uint32_t vhdBytesPerLine(VHD_BUFFERPACKING pack, int w) noexcept
{
  switch(pack)
  {
    case VHD_BUFPACK_VIDEO_YUV422_8:  return std::uint32_t(w) * 2u;
    case VHD_BUFPACK_VIDEO_YUV422_10: return std::uint32_t((w + 47) / 48) * 128u;
    case VHD_BUFPACK_VIDEO_RGB_32:    return std::uint32_t(w) * 4u;
    case VHD_BUFPACK_VIDEO_RGB_24:    return std::uint32_t(w) * 3u;
    default:                          return std::uint32_t(w) * 2u;
  }
}

} // namespace Gfx::Deltacast
