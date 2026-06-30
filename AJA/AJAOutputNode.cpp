#include "AJAOutputNode.hpp"

#include <AJA/AjaOutputBackend.hpp>

#include <memory>

namespace Gfx::AJA
{

AJANode::AJANode(const AJAOutputSettings& settings)
    : score::gfx::DirectVideoOutputNode{
          std::make_unique<AjaOutputBackend>(settings)}
{
}

} // namespace Gfx::AJA
