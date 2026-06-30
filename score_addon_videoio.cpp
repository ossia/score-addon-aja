#include "score_addon_videoio.hpp"

#include <VideoInput.hpp>
#include <VideoOutput.hpp>

#include <score/plugins/FactorySetup.hpp>

#include <score_plugin_gfx.hpp>

score_addon_videoio::score_addon_videoio()
{
  qRegisterMetaType<Gfx::VideoIO::VideoOutputSettings>();
  qRegisterMetaType<Gfx::VideoIO::VideoInputSettings>();
}

score_addon_videoio::~score_addon_videoio() = default;

std::vector<score::InterfaceBase*> score_addon_videoio::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  // One unified "Direct Video I/O" device per direction; each branches on the
  // detected card via per-vendor enumerators (AJA + DeckLink, #if-gated inside).
  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory,
         Gfx::VideoIO::VideoOutputProtocolFactory,
         Gfx::VideoIO::VideoInputProtocolFactory>>(ctx, key);
}

auto score_addon_videoio::required() const -> std::vector<score::PluginKey>
{
  return {score_plugin_gfx::static_key()};
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_videoio)
