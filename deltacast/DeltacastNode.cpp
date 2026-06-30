#include "DeltacastNode.hpp"

#include <memory>

namespace Gfx::Deltacast
{

DeltacastNode::DeltacastNode(const DeltacastOutputSettings& settings)
    : score::gfx::DirectVideoOutputNode{
          std::make_unique<DeltacastOutputBackend>(settings)}
{
}

} // namespace Gfx::Deltacast
