#pragma once
#include <AJA/AJAOutput.hpp>
#include <Gfx/Graph/DirectVideoOutputNode.hpp>

#include <score_addon_videoio_export.h>

namespace Gfx::AJA
{

/**
 * @brief AJA SDI playout node.
 *
 * Thin vendor wrapper over the shared score::gfx::DirectVideoOutputNode: all the
 * QRhi + render loop + createOutput orchestration lives in the base; AJA only
 * supplies an AjaOutputBackend (device open, routing, VPID/HDR, encoder format,
 * GPU-direct strategies, VBI pacing).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT AJANode final : score::gfx::DirectVideoOutputNode
{
  explicit AJANode(const AJAOutputSettings& settings);
};

} // namespace Gfx::AJA
