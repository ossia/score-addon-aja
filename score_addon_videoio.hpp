#pragma once
#include <score/application/ApplicationContext.hpp>
#include <score/plugins/Interface.hpp>
#include <score/plugins/qt_interfaces/FactoryInterface_QtInterface.hpp>
#include <score/plugins/qt_interfaces/PluginRequirements_QtInterface.hpp>

#include <vector>

/**
 * @brief Unified "Direct Video I/O" addon — professional capture-card playout
 *        and capture across vendors (DeckLink today; Bluefish/Deltacast/Magewell
 *        next), all behind one device that branches on the detected card.
 *
 * The vendor backends implement score::gfx::DirectVideoOutputBackend /
 * DMACaptureBackend; this addon hardcodes the compiled-in vendor set (gated by
 * SCORE_HAS_<VENDOR>) and dispatches on a vendor tag — no registry.
 */
class score_addon_videoio final
    : public score::Plugin_QtInterface
    , public score::FactoryInterface_QtInterface
{
  SCORE_PLUGIN_METADATA(1, "5b1f6a2e-9c3d-4e8a-bf21-7d6c4a0e9f55")

public:
  score_addon_videoio();
  ~score_addon_videoio() override;

private:
  std::vector<score::InterfaceBase*> factories(
      const score::ApplicationContext& ctx,
      const score::InterfaceKey& key) const override;

  std::vector<score::PluginKey> required() const override;
};
