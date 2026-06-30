#pragma once
#include <AJA/Tier3Common.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/interop/GpuRingBuffer.hpp>
#include <Gfx/Graph/interop/InteropFence.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhivulkan_p.h>

#include <QDebug>

namespace Gfx::AJA
{

/**
 * @brief Vulkan tier-3 RDMA output for AJA — STUB.
 *
 * Phase-2 unblocked the primitives (`vkinterop::createExportableBuffer`,
 * `exportMemoryHandle`, `cuda_p2p_import_vulkan_buffer`). The remaining
 * gap is the dispatch side: QRhi compute encoders bind a `QRhiBuffer`
 * SSBO, but our externally-allocated VkBuffer is not a QRhiBuffer.
 * Two paths forward (option (a) is the chosen plan):
 *
 *   (a) `QRhi::beginExternal` + hand-rolled Vulkan compute, loading the
 *       V210Compute / UYVYCompute / BGRACompute SPIR-V into a stand-
 *       alone VkPipeline. ~250 lines of new Vulkan plumbing; the
 *       `GpuDirectOutput` core doesn't change.
 *
 *   (b) QRhi private-API patch on `QVkBuffer` to inject
 *       `VkExternalMemoryBufferCreateInfo` + `VkExportMemoryAllocateInfo`
 *       into VMA's allocate-info. ABI-fragile across Qt versions.
 *
 * Until option (a) lands `init()` returns false; AJANode falls back to
 * encoder + CPU staging on Vulkan output (works today).
 */
struct RdmaInteropVulkanTier3 final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropVulkanTier3(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};

  const char* name() const noexcept override { return "RDMA-Vulkan/T3"; }

  static bool isSupported(QRhi* rhi, NTV2FrameBufferFormat fmt, int width)
  {
    return rhi && rhi->isFeatureSupported(QRhi::Compute)
           && tier3SupportsFormat(fmt, width);
  }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    if(!isSupported(c.rhi, m_targetFormat, c.width) || !c.state)
      return false;
    qDebug() << "AJA RDMA(Vulkan/T3): not implemented yet — needs "
                "beginExternal + raw Vulkan compute against external "
                "VkBuffer. Falling back to encoder + CPU staging.";
    return false;
  }

  void release() override { }
  void encodeFrame(QRhiCommandBuffer&) override { }
  void* prepareNextFrame() override { return nullptr; }
};

} // namespace Gfx::AJA
