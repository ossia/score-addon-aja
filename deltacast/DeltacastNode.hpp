#pragma once
#include <deltacast/DeltacastOutputBackend.hpp>

#include <Gfx/Graph/DirectVideoOutputNode.hpp>

#include <score_addon_videoio_export.h>

namespace Gfx::Deltacast
{

/**
 * @brief DELTACAST playout node — thin wrapper over DirectVideoOutputNode
 * supplying a DeltacastOutputBackend (the base owns QRhi + render loop).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT DeltacastNode final
    : score::gfx::DirectVideoOutputNode
{
  explicit DeltacastNode(const DeltacastOutputSettings& settings);
};

} // namespace Gfx::Deltacast
