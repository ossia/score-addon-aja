#include "DeckLinkNode.hpp"

#include <memory>

namespace Gfx::DeckLink
{

DeckLinkNode::DeckLinkNode(const DeckLinkOutputSettings& settings)
    : score::gfx::DirectVideoOutputNode{
          std::make_unique<DeckLinkOutputBackend>(settings)}
{
}

} // namespace Gfx::DeckLink
