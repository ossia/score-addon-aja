#pragma once
#include <decklink/DeckLinkOutputBackend.hpp>

#include <Gfx/Graph/DirectVideoOutputNode.hpp>

#include <score_addon_videoio_export.h>

namespace Gfx::DeckLink
{

/**
 * @brief DeckLink playout node.
 *
 * Thin vendor wrapper over score::gfx::DirectVideoOutputNode: the base owns the
 * QRhi + render loop + createOutput orchestration; DeckLink only supplies a
 * DeckLinkOutputBackend (scheduled playback, zero-copy host frames, pacing).
 */
struct SCORE_ADDON_VIDEOIO_EXPORT DeckLinkNode final
    : score::gfx::DirectVideoOutputNode
{
  explicit DeckLinkNode(const DeckLinkOutputSettings& settings);
};

} // namespace Gfx::DeckLink
