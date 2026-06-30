#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::Deltacast
{

/// A DELTACAST board as seen by the unified Direct Video I/O enumerator.
struct DeviceInfo
{
  int index{};            ///< board index passed to VHD_OpenBoardHandle
  std::string displayName;
  bool canOutput{};       ///< has >=1 TX channel
  bool canInput{};        ///< has >=1 RX channel
};

/// Probe the VideoMaster runtime (VHD_GetApiInfo). False if the DLL/driver is
/// missing. Safe to call repeatedly.
bool ensureVhdInit() noexcept;

/// Enumerate installed DELTACAST boards (RX/TX channel counts -> can*).
std::vector<DeviceInfo> enumerateDevices();

} // namespace Gfx::Deltacast
