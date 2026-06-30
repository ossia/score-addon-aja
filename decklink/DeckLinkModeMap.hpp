#pragma once
#include <decklink/DeckLink.hpp>

#include <QString>

namespace Gfx::DeckLink
{

/// Neutral video-format token ("1080p5994", "2160p60", AJA's "UHD*" spellings)
/// -> BMDDisplayMode. Defaults to 1080p59.94 for unknown tokens.
inline BMDDisplayMode bmdModeFromToken(const QString& f) noexcept
{
  if(f == "1080p2398") return bmdModeHD1080p2398;
  if(f == "1080p24")   return bmdModeHD1080p24;
  if(f == "1080p25")   return bmdModeHD1080p25;
  if(f == "1080p2997") return bmdModeHD1080p2997;
  if(f == "1080p30")   return bmdModeHD1080p30;
  if(f == "1080p50")   return bmdModeHD1080p50;
  if(f == "1080p5994") return bmdModeHD1080p5994;
  if(f == "1080p60")   return bmdModeHD1080p6000;
  if(f == "1080i50")   return bmdModeHD1080i50;
  if(f == "1080i5994") return bmdModeHD1080i5994;
  if(f == "1080i60")   return bmdModeHD1080i6000;
  if(f == "720p50")    return bmdModeHD720p50;
  if(f == "720p5994")  return bmdModeHD720p5994;
  if(f == "720p60")    return bmdModeHD720p60;
  if(f == "2160p25" || f == "UHD25")     return bmdMode4K2160p25;
  if(f == "2160p2997" || f == "UHD2997") return bmdMode4K2160p2997;
  if(f == "2160p30" || f == "UHD30")     return bmdMode4K2160p30;
  if(f == "2160p50" || f == "UHD50")     return bmdMode4K2160p50;
  if(f == "2160p5994" || f == "UHD5994") return bmdMode4K2160p5994;
  if(f == "2160p60" || f == "UHD60")     return bmdMode4K2160p60;
  return bmdModeHD1080p5994;
}

/// Neutral pixel-format token (YCbCr8 / YCbCr10 / RGB8|BGRA8 / RGB10)
/// -> BMDPixelFormat. Defaults to 8-bit YUV.
inline BMDPixelFormat bmdPixelFromToken(const QString& p) noexcept
{
  if(p == "YCbCr8")  return bmdFormat8BitYUV;
  if(p == "YCbCr10") return bmdFormat10BitYUV;
  if(p == "RGB8" || p == "BGRA8") return bmdFormat8BitBGRA;
  if(p == "RGB10")   return bmdFormat10BitRGB;
  return bmdFormat8BitYUV;
}

} // namespace Gfx::DeckLink
