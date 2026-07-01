#include "BluefishNode.hpp"

#include <bluefish/BluefishOutputBackend.hpp>

#include <memory>

namespace Gfx::Bluefish
{

BluefishNode::BluefishNode(const BluefishOutputSettings& settings)
    : score::gfx::DirectVideoOutputNode{
          std::make_unique<BluefishOutputBackend>(settings)}
{
}

} // namespace Gfx::Bluefish
