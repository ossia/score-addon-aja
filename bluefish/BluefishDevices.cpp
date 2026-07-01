#include <bluefish/BluefishDevices.hpp>

#include <bluefish/Bluefish.hpp>
#include <bluefish/BluefishFormats.hpp>
#include <bluefish/BluefishSettings.hpp>

#include <QString>

#include <string>

namespace Gfx::Bluefish
{

bool ensureBlueInit() noexcept
{
  // bfcUtilsGetDeviceCount() talks to the driver; a return >= 0 means the
  // runtime resolved. (No card returns 0, still a valid "initialised" state.)
  return bfcUtilsGetDeviceCount() >= 0;
}

std::vector<DeviceInfo> enumerateDevices()
{
  std::vector<DeviceInfo> out;
  const BLUE_S32 count = bfcUtilsGetDeviceCount();
  if(count < 1)
    return out;

  for(BLUE_S32 id = 1; id <= count; ++id)
  {
    blue_device_info info{};
    if(BLUE_FAIL(bfcUtilsGetDeviceInfo(id, &info)))
      continue;

    DeviceInfo dev;
    dev.index = static_cast<int>(id);
    dev.canInput = info.SdiStreamCountInput > 0;
    dev.canOutput = info.SdiStreamCountOutput > 0;

    const char* name = bfcUtilsGetStringForBlueProductId(info.FirmwareType);
    dev.displayName = name ? std::string(name) : ("Bluefish444 card " + std::to_string(id));
    dev.displayName += " (" + std::to_string(id) + ")";

    out.push_back(std::move(dev));
  }
  return out;
}

// SDK-free wrappers (declared in BluefishSettings.hpp) forwarding to the
// SDK-based inline helpers; keeps the BlueVelvetC __declspec headers out of the
// neutral VideoOutput.cpp / VideoInput.cpp translation units.
std::uint32_t videoModeExtFromToken(const QString& token) noexcept
{
  return static_cast<std::uint32_t>(videoModeExtFromTokenBlue(token));
}

std::uint32_t memFmtFromToken(const QString& token) noexcept
{
  return static_cast<std::uint32_t>(memFmtFromTokenBlue(token));
}

} // namespace Gfx::Bluefish
