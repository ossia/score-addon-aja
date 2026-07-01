#pragma once
#include <bluefish/BluefishSettings.hpp>

#include <Gfx/Graph/DirectVideoOutputNode.hpp>

#include <score_addon_videoio_export.h>

namespace Gfx::Bluefish
{

/**
 * @brief Bluefish444 playout node — thin wrapper over DirectVideoOutputNode
 * supplying a BluefishOutputBackend (the base owns QRhi + render loop).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT BluefishNode final
    : score::gfx::DirectVideoOutputNode
{
  explicit BluefishNode(const BluefishOutputSettings& settings);
};

} // namespace Gfx::Bluefish
