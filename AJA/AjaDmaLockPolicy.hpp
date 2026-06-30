#pragma once
#include <AJA/AjaDmaLock.hpp>

#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief DMA-lock policy for the shared DVP shims (score::gfx::interop::Dvp*).
 *
 * Pins the DVP sysmem slot for the AJA card's DMA engine via
 * CNTV2Card::DMABufferLock (paged host-memory DMA, inRDMA=false) — the exact
 * behaviour the standalone AJA DVP shims had inline.
 */
struct AjaDmaLockPolicy
{
  CNTV2Card* card{};

  bool valid() const noexcept { return card != nullptr; }
  bool lock(const void* ptr, std::uint32_t bytes) noexcept
  {
    return ajaDmaLock(card, ptr, bytes, /*rdma=*/false);
  }
  void unlock(const void* ptr, std::uint32_t bytes) noexcept
  {
    ajaDmaUnlock(card, ptr, bytes);
  }
};

} // namespace Gfx::AJA
