#pragma once
/**
 * @file BluefishSettings.hpp
 * @brief SDK-free Bluefish playout/capture settings + token conversions.
 *
 * Deliberately does NOT include the BlueVelvetC SDK headers: those use MSVC
 * `__declspec(dllimport, deprecated(...))`, which clang only accepts with
 * `-fdeclspec` — a flag incompatible with score's shared precompiled header. By
 * keeping the SDK out of this header, the neutral wiring (VideoOutput.cpp /
 * VideoInput.cpp) and the node headers stay PCH-clean; only the bluefish .cpp
 * translation units pull in the SDK (built with -fdeclspec, PCH disabled).
 *
 * The EVideoModeExt / EMemoryFormat values are carried as plain uint32_t and
 * static_cast back to the SDK enums inside the backends.
 */

#include <cstdint>

class QString;

namespace Gfx::Bluefish
{

struct BluefishOutputSettings
{
  int deviceIndex{1};                 ///< 1-based Bluefish device id
  std::uint32_t videoModeExt{1055};   ///< EVideoModeExt (1055 = VID_FMT_EXT_1080P_5994)
  std::uint32_t memoryFormat{2};      ///< EMemoryFormat (2 = MEM_FMT_YUVS / MEM_FMT_BV8)
};

struct BluefishInputSettings
{
  int deviceIndex{1};                 ///< 1-based Bluefish device id
  std::uint32_t memoryFormat{2};      ///< EMemoryFormat (2 = MEM_FMT_YUVS)
  // The incoming video mode is auto-detected (bfcUtilsGetRecommendedSetupInfoInput);
  // the widget's "expected format" is only a UI hint.
};

/// Neutral video-format token ("1080p5994", ...) -> EVideoModeExt (as uint32_t).
/// Defined in BluefishDevices.cpp (an SDK translation unit). Defaults to
/// 1080p5994 for unknown tokens.
std::uint32_t videoModeExtFromToken(const QString& token) noexcept;

/// Pixel-format token ("YCbCr8"/"YCbCr10"/"RGBA"/"RGB8") -> EMemoryFormat
/// (as uint32_t). Defined in BluefishDevices.cpp. Defaults to 8-bit YUV (YUVS).
std::uint32_t memFmtFromToken(const QString& token) noexcept;

} // namespace Gfx::Bluefish
