#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::Bluefish
{

/// A Bluefish444 card as seen by the unified Direct Video I/O enumerator.
///
/// NOTE: Bluefish device IDs are 1-based (bfcUtilsGetDeviceInfo / bfcAttach take
/// 1..bfcUtilsGetDeviceCount()), unlike the 0-based AJA/DeckLink/Deltacast
/// indices. We store that 1-based id in `index` and pass it straight through to
/// bfcAttach, so the neutral settings never need a special case.
struct DeviceInfo
{
  int index{};            ///< 1-based Bluefish device id (bfcAttach)
  std::string displayName;
  bool canOutput{};       ///< SdiStreamCountOutput > 0
  bool canInput{};        ///< SdiStreamCountInput > 0
};

/// Probe the BlueVelvetC runtime. False if the driver/DLL is missing. Safe to
/// call repeatedly.
bool ensureBlueInit() noexcept;

/// Enumerate installed Bluefish444 cards (RX/TX stream counts -> can*).
std::vector<DeviceInfo> enumerateDevices();

} // namespace Gfx::Bluefish
