#include "score_addon_videoio.hpp"

#include <Device/Protocol/ProtocolFactoryInterface.hpp>

#include <score/plugins/FactorySetup.hpp>

#include <score_plugin_gfx.hpp>

#if defined(SCORE_HAS_AJA)
#include <AJA/AJAInput.hpp>
#include <AJA/AJAOutput.hpp>
#endif

score_addon_videoio::score_addon_videoio()
{
#if defined(SCORE_HAS_AJA)
  qRegisterMetaType<Gfx::AJA::AJAOutputSettings>();
  qRegisterMetaType<Gfx::AJA::AJAInputSettings>();
#endif
}

score_addon_videoio::~score_addon_videoio() = default;

std::vector<score::InterfaceBase*> score_addon_videoio::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  // Parity checkpoint: the AJA device/GPU code is relocated verbatim and its
  // factories registered as-is. The unified Direct Video I/O device (with
  // per-vendor enumerators + DeckLink) replaces these in the next pass.
  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory
#if defined(SCORE_HAS_AJA)
         ,
         Gfx::AJA::AJAProtocolFactory,
         Gfx::AJA::AJAInputProtocolFactory
#endif
         >>(ctx, key);
}

auto score_addon_videoio::required() const -> std::vector<score::PluginKey>
{
  return {score_plugin_gfx::static_key()};
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_videoio)
