#pragma once

/**
 * @file VideoIOCaps.hpp
 * @brief Card-capability filters for the unified Direct Video I/O settings UI.
 *
 * The settings widgets keep a hardcoded master list of (label, token) format /
 * pixel-format choices (the superset we understand). These helpers take that
 * master list and return only the entries the *selected* card actually supports,
 * queried live from the vendor SDK. Tokens are reused as-is (parseAjaVideoFormat
 * / bmdModeFromToken) — no inverse maps, no SDK types in the neutral settings.
 *
 * FAIL OPEN: on any failure (no device, can't open, empty/failed query, or the
 * vendor isn't compiled in) the full master list is returned unchanged. A failed
 * capability query must never leave a combo empty or wrongly restricted.
 */

#include <VideoIOSettings.hpp>

#include <QString>

#include <utility>
#include <vector>

#if defined(SCORE_HAS_AJA)
#include <AJA/AjaFormatMap.hpp>

#include <ntv2card.h>
#endif

#if defined(SCORE_HAS_DECKLINK)
#include <decklink/DeckLink.hpp>
#include <decklink/DeckLinkComPtr.hpp>
#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkModeMap.hpp>

#include <set>
#endif

namespace Gfx::VideoIO::caps
{

using FormatList = std::vector<std::pair<QString, QString>>; ///< (label, token)

#if defined(SCORE_HAS_DECKLINK)
/// Collect the set of BMDDisplayModes the device at `index` reports for the
/// given direction. Empty on any failure (caller falls back to the master list).
inline std::set<BMDDisplayMode> deckLinkSupportedModes(int index, bool forOutput)
{
  using Gfx::DeckLink::ComPtr;
  std::set<BMDDisplayMode> modes;
  if(index < 0 || !Gfx::DeckLink::ensureComInit())
    return modes;
  ComPtr<IDeckLink> dev = Gfx::DeckLink::openDevice(index);
  if(!dev)
    return modes;

  ComPtr<IDeckLinkDisplayModeIterator> it;
  if(forOutput)
  {
    ComPtr<IDeckLinkOutput> output;
    if(dev->QueryInterface(IID_IDeckLinkOutput, output.putVoid()) != S_OK
       || !output)
      return modes;
    if(output->GetDisplayModeIterator(it.put()) != S_OK || !it)
      return modes;
  }
  else
  {
    ComPtr<IDeckLinkInput> input;
    if(dev->QueryInterface(IID_IDeckLinkInput, input.putVoid()) != S_OK || !input)
      return modes;
    if(input->GetDisplayModeIterator(it.put()) != S_OK || !it)
      return modes;
  }

  ComPtr<IDeckLinkDisplayMode> m;
  while(it->Next(m.put()) == S_OK && m)
  {
    modes.insert(m->GetDisplayMode());
    m.reset();
  }
  return modes;
}
#endif

/// Filter the master video-format list to what the selected card supports.
/// @param forOutput true => playout (IDeckLinkOutput); false => capture.
inline FormatList filterVideoFormats(
    Vendor vendor, int deviceIndex, bool forOutput, const FormatList& master)
{
#if defined(SCORE_HAS_AJA)
  if(vendor == Vendor::AJA)
  {
    if(deviceIndex < 0)
      return master;
    CNTV2Card card(static_cast<UWord>(deviceIndex));
    if(!card.IsOpen())
      return master;
    FormatList kept;
    for(const auto& e : master)
      if(card.features().CanDoVideoFormat(
             Gfx::AJA::parseAjaVideoFormat(e.second, 0.0)))
        kept.push_back(e);
    return kept.empty() ? master : kept;
  }
#endif
#if defined(SCORE_HAS_DECKLINK)
  if(vendor == Vendor::DeckLink)
  {
    const auto modes = deckLinkSupportedModes(deviceIndex, forOutput);
    if(modes.empty())
      return master;
    FormatList kept;
    for(const auto& e : master)
      if(modes.count(Gfx::DeckLink::bmdModeFromToken(e.second)) > 0)
        kept.push_back(e);
    return kept.empty() ? master : kept;
  }
#endif
  (void)vendor;
  (void)deviceIndex;
  (void)forOutput;
  return master;
}

/// Filter the master pixel-format list. AJA queries the card; other vendors
/// pass the master list through unchanged (per-mode pixel querying isn't worth
/// the SDK-signature risk on DeckLink).
inline FormatList filterPixelFormats(
    Vendor vendor, int deviceIndex, const FormatList& master)
{
#if defined(SCORE_HAS_AJA)
  if(vendor == Vendor::AJA)
  {
    if(deviceIndex < 0)
      return master;
    CNTV2Card card(static_cast<UWord>(deviceIndex));
    if(!card.IsOpen())
      return master;
    FormatList kept;
    for(const auto& e : master)
      if(card.features().CanDoFrameBufferFormat(
             Gfx::AJA::parseAjaPixelFormat(e.second)))
        kept.push_back(e);
    return kept.empty() ? master : kept;
  }
#endif
  (void)vendor;
  (void)deviceIndex;
  return master;
}

} // namespace Gfx::VideoIO::caps
