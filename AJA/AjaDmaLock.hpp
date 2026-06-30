#pragma once

/**
 * @file AjaDmaLock.hpp
 * @brief Thin wrappers over CNTV2Card::DMABufferLock / DMABufferUnlock.
 *
 * Every AJA capture/output interop strategy page-locks its DMA buffers with
 * the same boilerplate — `reinterpret_cast<const ULWord*>(ptr)` +
 * `static_cast<ULWord>(bytes)` + the `inMap=true` convention — repeated across
 * a dozen files. These helpers centralize the casts and the convention so the
 * per-strategy lock/unlock loops read intention-first, and a null card / null
 * pointer is handled uniformly.
 *
 * The `inRDMA` flag stays an explicit argument: it is the one genuine
 * per-strategy difference (false for sysmem/paged DMA, true for GPUDirect
 * P2P into VRAM).
 */

#include <ntv2card.h>

#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief Page-lock @p bytes at @p ptr for AJA DMA.
 * @param rdma true => GPUDirect RDMA (P2P into a GPU device pointer);
 *             false => paged host-memory DMA.
 * @return true on success; false if @p card or @p ptr is null, or the lock
 *         failed.
 */
[[nodiscard]] inline bool ajaDmaLock(
    CNTV2Card* card, const void* ptr, std::uint32_t bytes, bool rdma) noexcept
{
  return card && ptr
         && card->DMABufferLock(
                reinterpret_cast<const ULWord*>(ptr),
                static_cast<ULWord>(bytes), /*inMap=*/true, /*inRDMA=*/rdma);
}

/// Unlock a buffer previously locked with ajaDmaLock(). No-op if null.
inline void ajaDmaUnlock(
    CNTV2Card* card, const void* ptr, std::uint32_t bytes) noexcept
{
  if(card && ptr)
    card->DMABufferUnlock(
        reinterpret_cast<const ULWord*>(ptr), static_cast<ULWord>(bytes));
}

} // namespace Gfx::AJA
