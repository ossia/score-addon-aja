#pragma once
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>

#include <ntv2card.h>

#include <QtGui/private/qrhi_p.h>

#if QT_CONFIG(opengl)
#include <Gfx/Graph/interop/GLCaptureUpload.hpp>

#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>
#endif

#include <QDebug>

#include <array>
#include <cstdint>
#include <vector>

namespace Gfx::AJA
{

/**
 * @brief Universal CPU-staging capture: AJA SDI -> page-locked sysmem ->
 *        QRhi texture. Works on every backend.
 *
 * The portable fallback for the capture path: AJA DMAs the captured frame into
 * a page-locked host buffer (DMABufferLock inRDMA=false); the render thread
 * uploads it into the decoder's input texture. The upload picks the cheapest
 * path for the active backend:
 *
 *   - OpenGL: a single raw glTexSubImage2D straight from the sysmem slot into
 *     the texture (one driver copy).
 *   - Vulkan / Metal / D3D: QRhiResourceUpdateBatch::uploadTexture (the only
 *     upload API available with no raw call to hand inside acquireForRender) —
 *     this is what makes SDI capture work at all on those backends.
 *
 * The raw-GL path matters at scale: QRhi's portable uploadTexture copies the
 * frame into batch-owned staging before the driver's own copy (one extra
 * full-frame memcpy), measurably slower on GL (~17% per upload at 1080p,
 * scaling with frame size). So GL keeps the raw path; the portable path is
 * used only where there is no alternative.
 *
 * The decoder's input-texture format already matches the AJA byte order
 * (BGRA8 for an ARGB framestore, RGBA8 otherwise — chosen by the AJAInputNode
 * decoder), so neither upload path needs a channel swizzle; the PackedDecoder
 * / V210Decoder / UYVY422Decoder shader unpacks UYVY / v210 / RGBA in the
 * fragment stage.
 *
 * SCORE_AJA_FORCE_PORTABLE_UPLOAD=1 forces the portable path even on GL (to
 * benchmark / validate it against the raw path on the same backend).
 */
struct CaptureInteropCpu final : score::gfx::interop::GpuDirectCaptureStrategy
{
  CaptureInteropCpu(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}
      , m_pixelFormat{pixfmt}
  {
  }

  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

#if QT_CONFIG(opengl)
  // Non-null when the raw-GL fast path is in use (GL backend, not forced
  // portable). The portable path leaves this null and uploads via the batch.
  QOpenGLContext* m_glCtx{};
#endif

  static constexpr std::size_t kSlotCount = 3;
  std::array<std::vector<uint8_t>, kSlotCount> m_slots;
  std::array<bool, kSlotCount> m_dmaLocked{};
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  const char* name() const noexcept override
  {
#if QT_CONFIG(opengl)
    return m_glCtx ? "CPU-GL" : "CPU-QRhi";
#else
    return "CPU-QRhi";
#endif
  }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !m_card || !cfg.outputTexture)
      return false;

    if(!score::gfx::interop::validateCaptureTextureBytes(
           cfg.outputTexture, cfg.frameByteSize, "AJA CPU-IN:"))
      return false;

#if QT_CONFIG(opengl)
    // Raw-GL fast path: grab the QRhi GL context unless forced portable.
    if(cfg.rhi->backend() == QRhi::OpenGLES2
       && !qEnvironmentVariableIsSet("SCORE_AJA_FORCE_PORTABLE_UPLOAD"))
    {
      if(auto* native
         = static_cast<const QRhiGles2NativeHandles*>(cfg.rhi->nativeHandles());
         native && native->context)
        m_glCtx = native->context;
    }
#endif

    for(std::size_t i = 0; i < kSlotCount; ++i)
    {
      m_slots[i].assign(cfg.frameByteSize, 0);
      // Page-lock (paged, not RDMA) so AJA's AutoCirculate DMA into the host
      // buffer doesn't re-pin pages every frame.
      if(ajaDmaLock(m_card, m_slots[i].data(), cfg.frameByteSize, /*rdma=*/false))
        m_dmaLocked[i] = true;
    }
    return true;
  }

  void release() override
  {
    // NB: do NOT DMABufferUnlock here — the capture session's close() does
    // DMABufferUnlockAll, and by the time this strategy is released during
    // graph teardown the CNTV2Card may already be gone. Just drop the host
    // buffers; the unlock-all clears the driver's locked-region table.
    for(std::size_t i = 0; i < kSlotCount; ++i)
    {
      m_dmaLocked[i] = false;
      m_slots[i].clear();
    }
#if QT_CONFIG(opengl)
    m_glCtx = nullptr;
#endif
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override { return kSlotCount; }

  void* slotBuffer(std::size_t i) const noexcept override
  {
    return (i < kSlotCount) ? const_cast<uint8_t*>(m_slots[i].data()) : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    if(i >= kSlotCount)
      return false;
    m_publisher.publish(i);
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override { return cfg.outputTexture; }

  // Raw-API path unused: the upload goes through the batch overload.
  void acquireForRender() override { }

  void acquireForRender(QRhiResourceUpdateBatch& res) override
  {
    const int slotIdx = m_publisher.consume();
    if(slotIdx < 0 || static_cast<std::size_t>(slotIdx) >= kSlotCount)
      return;
    const void* src = m_slots[static_cast<std::size_t>(slotIdx)].data();
    const bool bgra = (m_pixelFormat == AJAInputPixelFormat::ARGB);

#if QT_CONFIG(opengl)
    if(m_glCtx)
    {
      // Raw-GL fast path: one glTexSubImage2D, no staging copy.
      score::gfx::interop::uploadClientToGLTexture(
          *m_glCtx, *cfg.outputTexture, src, bgra);
      return;
    }
#endif

    // Portable path: backend-neutral QRhi upload. The sysmem slot holds one
    // tightly-packed frame matching the texture's byte layout (stride=width*4).
    (void)bgra; // texture format already encodes channel order; no swizzle.
    const auto sz = cfg.outputTexture->pixelSize();
    QRhiTextureSubresourceUploadDescription sub(src, cfg.frameByteSize);
    sub.setDataStride(static_cast<quint32>(sz.width()) * 4u);
    QRhiTextureUploadEntry entry{0, 0, sub};
    res.uploadTexture(cfg.outputTexture, QRhiTextureUploadDescription{entry});
  }

  void releaseAfterRender() override { }
};

} // namespace Gfx::AJA
