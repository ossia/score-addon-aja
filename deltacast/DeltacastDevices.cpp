#include <deltacast/DeltacastDevices.hpp>

#include <deltacast/Deltacast.hpp>

#include <string>

namespace Gfx::Deltacast
{

bool ensureVhdInit() noexcept
{
  ULONG ver = 0, nb = 0;
  return VHD_GetApiInfo(&ver, &nb) == VHDERR_NOERROR;
}

std::vector<DeviceInfo> enumerateDevices()
{
  std::vector<DeviceInfo> out;
  ULONG ver = 0, nb = 0;
  if(VHD_GetApiInfo(&ver, &nb) != VHDERR_NOERROR)
    return out;

  for(ULONG i = 0; i < nb; ++i)
  {
    HANDLE brd = nullptr;
    if(VHD_OpenBoardHandle(i, &brd, nullptr, 0) != VHDERR_NOERROR || !brd)
      continue;

    DeviceInfo info;
    info.index = static_cast<int>(i);
    ULONG nbRx = 0, nbTx = 0;
    VHD_GetBoardProperty(brd, VHD_CORE_BP_NB_RXCHANNELS, &nbRx);
    VHD_GetBoardProperty(brd, VHD_CORE_BP_NB_TXCHANNELS, &nbTx);
    info.canInput = nbRx > 0;
    info.canOutput = nbTx > 0;
    // A board-type -> model-name table is a later refinement; index is stable.
    info.displayName = "DELTACAST board " + std::to_string(i);

    VHD_CloseBoardHandle(brd);
    out.push_back(std::move(info));
  }
  return out;
}

} // namespace Gfx::Deltacast
