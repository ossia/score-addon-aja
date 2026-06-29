#include <decklink/DeckLinkDevices.hpp>

#include <QString>

namespace Gfx::DeckLink
{

bool ensureComInit() noexcept
{
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // S_FALSE = already initialised on this thread; RPC_E_CHANGED_MODE = already
  // initialised in a different (STA) model, which is fine for our use.
  return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

namespace
{
ComPtr<IDeckLinkIterator> makeIterator()
{
  ComPtr<IDeckLinkIterator> it;
  CoCreateInstance(
      CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
      it.putVoid());
  return it;
}

std::string bstrToUtf8(BSTR s)
{
  if(!s)
    return {};
  const auto qs = QString::fromWCharArray(
      reinterpret_cast<const wchar_t*>(s), int(SysStringLen(s)));
  return qs.toStdString();
}
} // namespace

std::vector<DeviceInfo> enumerateDevices()
{
  ensureComInit();
  std::vector<DeviceInfo> out;
  auto it = makeIterator();
  if(!it)
    return out;

  ComPtr<IDeckLink> dev;
  for(int idx = 0; it->Next(dev.put()) == S_OK; ++idx, dev.reset())
  {
    DeviceInfo info;
    info.index = idx;

    BSTR name = nullptr;
    if(dev->GetDisplayName(&name) == S_OK)
    {
      info.displayName = bstrToUtf8(name);
      SysFreeString(name);
    }

    ComPtr<IDeckLinkProfileAttributes> attr;
    if(dev->QueryInterface(IID_IDeckLinkProfileAttributes, attr.putVoid())
       == S_OK)
    {
      LONGLONG io = 0;
      if(attr->GetInt(BMDDeckLinkVideoIOSupport, &io) == S_OK)
      {
        info.canOutput = (io & bmdDeviceSupportsPlayback) != 0;
        info.canInput = (io & bmdDeviceSupportsCapture) != 0;
      }
      LONGLONG pid = 0;
      if(attr->GetInt(BMDDeckLinkPersistentID, &pid) == S_OK)
        info.persistentId = pid;
    }

    out.push_back(std::move(info));
  }
  return out;
}

ComPtr<IDeckLink> openDevice(int index)
{
  ensureComInit();
  auto it = makeIterator();
  if(!it)
    return {};

  ComPtr<IDeckLink> dev;
  for(int i = 0; it->Next(dev.put()) == S_OK; ++i)
  {
    if(i == index)
      return dev;
    dev.reset();
  }
  return {};
}

} // namespace Gfx::DeckLink
