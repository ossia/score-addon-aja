#include "score_addon_videoio.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <score_plugin_gfx.hpp>

score_addon_videoio::score_addon_videoio() = default;
score_addon_videoio::~score_addon_videoio() = default;

std::vector<score::InterfaceBase*> score_addon_videoio::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  // The unified Direct Video I/O protocol/device is added here once Split 3b
  // lands; for now the addon ships the vendor backends (compiled + ready).
  (void)ctx;
  (void)key;
  return {};
}

auto score_addon_videoio::required() const -> std::vector<score::PluginKey>
{
  return {score_plugin_gfx::static_key()};
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_videoio)
