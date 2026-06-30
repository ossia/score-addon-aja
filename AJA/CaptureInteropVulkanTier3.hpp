#pragma once
#include <AJA/AJAInput.hpp>
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <score/gfx/Vulkan.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhivulkan_p.h>

#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <QDebug>

#include <array>
#include <cstdint>

namespace Gfx::AJA
{

/**
 * @brief Vulkan tier-3 zero-copy capture (Design B: CUDA buffer->image copy).
 *
 * The symmetric inverse of CaptureInteropGLTier3 for the Vulkan backend. Unlike
 * GL — where the AJA-DMA'd buffer is uploaded into the decoder's texture with a
 * raw glTexSubImage2D on the render thread — Vulkan has no raw upload call to
 * hand inside acquireForRender(), so the per-frame copy is done on the CAPTURE
 * thread via CUDA, straight into the renderer texture's memory:
 *
 *   init (render thread):
 *     - allocate an exportable VkImage matching the decoder's input texture,
 *       import it into CUDA as a CUarray, and adopt it into QRhi via
 *       QRhiTexture::createFrom — this IS the texture the decoder samples
 *       (AJAInputNode swaps it in for its decoder's sampler).
 *     - per slot: allocate an exportable VkBuffer, import into CUDA -> flat GPU
 *       pointer, AJA DMABufferLock(inRDMA=true) so AutoCirculate P2P-DMAs the
 *       captured frame straight into that VRAM.
 *
 *   ingestFrame (capture thread): cuda_p2p_copy_buffer_to_array copies the slot
 *     buffer -> the image's CUarray (stream-synced), then publishes the slot.
 *
 *   acquireForRender (render thread): nothing to upload — the image already
 *     holds the latest frame. (A VkCuda timeline semaphore would be the robust
 *     cross-API sync; the capture-thread streamSync is the pragmatic baseline.)
 *
 * Requires nvidia-peermem (AJA GPUDirect RDMA pin); on hosts without it the
 * DMABufferLock(inRDMA=true) fails and AJAInputNode falls back to CPU staging.
 * The Vulkan<->CUDA machinery (export + image-map + copy) is validated by
 * AJARoundtrip --vk-interop-probe on consumer GPUs.
 */
struct CaptureInteropVulkanTier3 final : score::gfx::interop::GpuDirectCaptureStrategy
{
  CaptureInteropVulkanTier3(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  score::gfx::vkinterop::VulkanCtx m_vk{};
  QVulkanDeviceFunctions* m_devFuncs{};
  VkQueue m_gfxQueue{VK_NULL_HANDLE};
  int m_gfxFamily{-1};

  CudaP2PContextHandle m_cudaCtx{};

  // Renderer-facing texture: an exportable VkImage, CUDA-mapped + QRhi-adopted.
  score::gfx::vkinterop::ExternalImage m_image{};
  void* m_imgArray{};               // CUarray (copy destination)
  CudaP2PImageHandle m_imgHandle{};
  QRhiTexture* m_ownedTex{};

  static constexpr std::size_t kSlotCount = 3;
  struct Slot
  {
    score::gfx::vkinterop::ExternalBuffer buf{};
    void* gpuPtr{};                 // flat CUDA device ptr (AJA DMA target)
    CudaP2PResourceHandle res{};
    bool dmaLocked{};
  };
  std::array<Slot, kSlotCount> m_slots{};
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  int m_texW{}, m_texH{};
  std::uint32_t m_rowBytes{};

  const char* name() const noexcept override { return "RDMA-Vulkan/T3"; }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !m_card || !cfg.outputTexture
       || cfg.rhi->backend() != QRhi::Vulkan)
      return false;
    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "AJA RDMA-IN(Vulkan/T3):"))
      return false;
    if(!cuda_p2p_available())
      return false;

    auto* h = static_cast<const QRhiVulkanNativeHandles*>(cfg.rhi->nativeHandles());
    QVulkanInstance* qInst = score::gfx::staticVulkanInstance(false);
    if(!h || !h->dev || !h->physDev || !qInst)
      return false;
    m_vk = {qInst->vkInstance(), h->physDev, h->dev, qInst};
    m_devFuncs = qInst->deviceFunctions(h->dev);
    m_gfxQueue = h->gfxQueue;
    m_gfxFamily = h->gfxQueueFamilyIdx;
    if(!m_devFuncs || !m_gfxQueue || m_gfxFamily < 0)
      return false;

    if(cuda_p2p_init(&m_cudaCtx) != CUDA_P2P_SUCCESS || !m_cudaCtx)
      return false;

    const QSize sz = cfg.outputTexture->pixelSize();
    m_texW = sz.width();
    m_texH = sz.height();
    m_rowBytes = std::uint32_t(m_texW) * 4u;
    const bool bgra = (cfg.outputTexture->format() == QRhiTexture::BGRA8);
    const VkFormat vkfmt = bgra ? VK_FORMAT_B8G8R8A8_UNORM
                                : VK_FORMAT_R8G8B8A8_UNORM;

    // 1. Exportable VkImage -> CUDA array -> QRhi-adopted texture.
    namespace vki = score::gfx::vkinterop;
    {
      vki::ExternalImageDesc d{};
      d.format = vkfmt;
      d.extent = {std::uint32_t(m_texW), std::uint32_t(m_texH), 1};
      d.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      d.tiling = VK_IMAGE_TILING_OPTIMAL;
      d.handleType = vki::kOpaqueHandleType;
      d.dedicated = true;
      auto img = vki::createExportableImage(m_vk, d);
      if(!img)
        return releaseFail("createExportableImage");
      m_image = *img;

      // The image starts UNDEFINED; transition to GENERAL once so QRhi (told
      // GENERAL via createFrom) and CUDA (which writes the memory) agree.
      if(!transitionImageToGeneral())
        return releaseFail("image layout transition");

      auto handle
          = vki::exportMemoryHandle(m_vk, m_image.memory, vki::kOpaqueHandleType);
      if(!handle || !handle->isValid())
        return releaseFail("exportMemoryHandle(image)");
      CudaP2PImageDesc cd{std::uint32_t(m_texW), std::uint32_t(m_texH), 1, 4,
                          CUDA_P2P_FORMAT_UNSIGNED_INT8, 0};
      if(cuda_p2p_import_vulkan_image(
             m_cudaCtx, handle->osHandle(),
             m_image.size, &cd, 0, &m_imgArray, &m_imgHandle)
             != CUDA_P2P_SUCCESS
         || !m_imgArray)
        return releaseFail("cuda_p2p_import_vulkan_image");

      m_ownedTex = cfg.rhi->newTexture(
          cfg.outputTexture->format(), sz, 1, QRhiTexture::UsedAsTransferSource);
      QRhiTexture::NativeTexture nt{
          reinterpret_cast<quint64>(m_image.image), VK_IMAGE_LAYOUT_GENERAL};
      if(!m_ownedTex->createFrom(nt))
        return releaseFail("QRhiTexture::createFrom");
    }

    // 2. Per-slot exportable VkBuffer -> CUDA -> AJA RDMA pin.
    for(auto& s : m_slots)
    {
      vki::ExternalBufferDesc d{
          cfg.frameByteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          vki::kOpaqueHandleType, true};
      auto b = vki::createExportableBuffer(m_vk, d);
      if(!b)
        return releaseFail("createExportableBuffer");
      s.buf = *b;
      auto handle
          = vki::exportMemoryHandle(m_vk, s.buf.memory, vki::kOpaqueHandleType);
      if(!handle || !handle->isValid())
        return releaseFail("exportMemoryHandle(buffer)");
      if(cuda_p2p_import_vulkan_buffer(
             m_cudaCtx, handle->osHandle(),
             cfg.frameByteSize, &s.gpuPtr, &s.res)
             != CUDA_P2P_SUCCESS
         || !s.gpuPtr)
        return releaseFail("cuda_p2p_import_vulkan_buffer");
      // The hardware-gated step: pin the GPU pointer for AJA GPUDirect RDMA.
      if(!ajaDmaLock(m_card, s.gpuPtr, cfg.frameByteSize, /*rdma=*/true))
        return releaseFail("DMABufferLock(inRDMA=true) — needs nvidia-peermem");
      s.dmaLocked = true;
    }
    return true;
  }

  void release() override
  {
    for(auto& s : m_slots)
    {
      if(s.dmaLocked)
        ajaDmaUnlock(m_card, s.gpuPtr, cfg.frameByteSize);
      s.dmaLocked = false;
      if(s.res && m_cudaCtx)
        cuda_p2p_release_buffer(m_cudaCtx, s.res);
      s.res = {};
      s.gpuPtr = nullptr;
      if(s.buf.buffer)
        score::gfx::vkinterop::destroyExternal(m_vk, s.buf);
    }
    if(m_imgHandle && m_cudaCtx)
      cuda_p2p_release_image(m_cudaCtx, m_imgHandle);
    m_imgHandle = {};
    m_imgArray = nullptr;
    delete m_ownedTex;
    m_ownedTex = nullptr;
    if(m_image.image)
      score::gfx::vkinterop::destroyExternal(m_vk, m_image);
    if(m_cudaCtx)
      cuda_p2p_shutdown(m_cudaCtx);
    m_cudaCtx = nullptr;
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override { return kSlotCount; }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    return i < kSlotCount ? m_slots[i].gpuPtr : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    // Capture thread: AJA P2P-wrote the frame into slot i's VkBuffer VRAM.
    // Copy it into the renderer texture's CUDA array, then publish.
    if(i >= kSlotCount || !m_imgArray)
      return false;
    if(cuda_p2p_copy_buffer_to_array(
           m_cudaCtx, m_slots[i].gpuPtr, m_imgArray, m_rowBytes,
           std::uint32_t(m_texH), m_rowBytes)
       != CUDA_P2P_SUCCESS)
      return false;
    m_publisher.publish(i);
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override
  {
    return m_ownedTex ? m_ownedTex : cfg.outputTexture;
  }

  // The image already holds the latest frame (CUDA copy done on capture
  // thread); nothing to do on the render thread.
  void acquireForRender() override { m_publisher.consume(); }
  void releaseAfterRender() override { }

private:
  bool releaseFail(const char* what)
  {
    qWarning() << "AJA RDMA-IN(Vulkan/T3):" << what << "failed";
    release();
    return false;
  }

  // One-time UNDEFINED -> GENERAL transition via a transient command buffer,
  // so the QRhi-adopted image is genuinely in the layout createFrom claims.
  bool transitionImageToGeneral()
  {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = std::uint32_t(m_gfxFamily);
    if(m_devFuncs->vkCreateCommandPool(m_vk.dev, &pci, nullptr, &pool)
       != VK_SUCCESS)
      return false;

    VkCommandBuffer cb{VK_NULL_HANDLE};
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    bool ok = m_devFuncs->vkAllocateCommandBuffers(m_vk.dev, &ai, &cb)
              == VK_SUCCESS;
    if(ok)
    {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      m_devFuncs->vkBeginCommandBuffer(cb, &bi);

      VkImageMemoryBarrier b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = m_image.image;
      b.subresourceRange
          = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      m_devFuncs->vkCmdPipelineBarrier(
          cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
          &b);
      m_devFuncs->vkEndCommandBuffer(cb);

      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      ok = m_devFuncs->vkQueueSubmit(m_gfxQueue, 1, &si, VK_NULL_HANDLE)
           == VK_SUCCESS;
      if(ok)
        m_devFuncs->vkQueueWaitIdle(m_gfxQueue);
      m_devFuncs->vkFreeCommandBuffers(m_vk.dev, pool, 1, &cb);
    }
    m_devFuncs->vkDestroyCommandPool(m_vk.dev, pool, nullptr);
    return ok;
  }
};

} // namespace Gfx::AJA
