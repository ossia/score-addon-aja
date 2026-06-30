#pragma once
#include <AJA/AjaDmaLock.hpp>

#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <nv_dvp_bridge.h>

#include <ntv2card.h>

#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>

#include <QDebug>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Gfx::AJA
{

/**
 * @brief OpenGL capture strategy via NVIDIA "GPUDirect for Video" (DVP).
 *
 * Same architecture as CaptureInteropD3D11Dvp but using DVP's OpenGL
 * binding (`dvpInitGLContext` / `dvpCreateGPUTextureGL`). The QRhi GL
 * context must be current on the calling thread when init() runs and
 * when the renderer-side acquire/release runs; AJAInputNode runs both
 * from the score render thread on which QRhi made the GL context
 * current.
 */
struct CaptureInteropGLDvp final : score::gfx::interop::GpuDirectCaptureStrategy
{
  CaptureInteropGLDvp(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  QOpenGLContext* m_glCtx{};

  NvDvpContextHandle m_dvpCtx{};
  NvDvpResourceHandle m_dvpTex{};
  bool m_threadStarted{};

  static constexpr std::size_t kSlotCount = 3;
  struct Slot
  {
    void* sysmem{};
    NvDvpResourceHandle dvpBuf{};
    bool dmaLocked{};
  };
  std::array<Slot, kSlotCount> m_slots{};

  uint32_t m_sysmemBytes{};
  uint32_t m_sysmemStrideBytes{};
  int m_texW{};
  int m_texH{};

  const char* name() const noexcept override { return "DVP-GL"; }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !m_card || !cfg.outputTexture)
      return false;

    auto* native
        = static_cast<const QRhiGles2NativeHandles*>(cfg.rhi->nativeHandles());
    if(!native || !native->context)
      return false;
    m_glCtx = native->context;

    // dvpInitGLContext binds DVP to the *current* GL context, but init() runs
    // outside a QRhi frame so nothing is current here. Make the QRhi GL
    // context current on the render state's offscreen surface first. If this
    // fails, DVP init would fail anyway (and with a misleading error), so bail
    // out with a clear message.
    if(!cfg.state || !cfg.state->surface)
    {
      qWarning() << "AJA DVP-IN(GL): no offscreen surface available to bind the "
                    "GL context; cannot init DVP";
      return false;
    }
    if(!m_glCtx->makeCurrent(cfg.state->surface))
    {
      qWarning() << "AJA DVP-IN(GL): makeCurrent() failed; cannot bind DVP to "
                    "the GL context";
      return false;
    }

    qDebug() << "AJA DVP-IN(GL): loading dvp.dll...";
    if(nv_dvp_init_gl(&m_dvpCtx) != NV_DVP_SUCCESS || !m_dvpCtx)
    {
      qWarning() << "AJA DVP-IN(GL): init failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return false;
    }
    if(nv_dvp_thread_begin(m_dvpCtx) != NV_DVP_SUCCESS)
    {
      release();
      return false;
    }
    m_threadStarted = true;
    qDebug() << "AJA DVP-IN(GL): dvp.dll loaded + GL context bound";

    const QSize texSize = cfg.outputTexture->pixelSize();
    m_texW = texSize.width();
    m_texH = texSize.height();
    m_sysmemStrideBytes = static_cast<uint32_t>(m_texW) * 4u;
    m_sysmemBytes = m_sysmemStrideBytes * static_cast<uint32_t>(m_texH);

    if(m_sysmemBytes != cfg.frameByteSize)
    {
      qWarning() << "AJA DVP-IN(GL): texture byte size" << m_sysmemBytes
                 << "!=" << cfg.frameByteSize;
      release();
      return false;
    }

    auto nt = cfg.outputTexture->nativeTexture();
    if(!nt.object)
    {
      release();
      return false;
    }
    const uint32_t glTexId = uint32_t(nt.object);

    NvDvpFormat dvpFmt = (m_pixelFormat == AJAInputPixelFormat::ARGB)
                              ? NV_DVP_FORMAT_BGRA8
                              : NV_DVP_FORMAT_RGBA8;

    if(nv_dvp_register_gl_texture(
           m_dvpCtx, glTexId, dvpFmt, uint32_t(m_texW), uint32_t(m_texH),
           &m_dvpTex)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP-IN(GL): register_gl_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      release();
      return false;
    }

    for(auto& slot : m_slots)
    {
      slot.sysmem = nv_dvp_aligned_alloc(m_sysmemBytes);
      if(!slot.sysmem)
      {
        release();
        return false;
      }
      std::memset(slot.sysmem, 0, m_sysmemBytes);
      if(!ajaDmaLock(m_card, slot.sysmem, m_sysmemBytes, /*rdma=*/false))
      {
        release();
        return false;
      }
      slot.dmaLocked = true;
      if(nv_dvp_register_sysmem_buffer(
             m_dvpCtx, slot.sysmem, dvpFmt, uint32_t(m_texW),
             uint32_t(m_texH), m_sysmemStrideBytes, &slot.dvpBuf)
         != NV_DVP_SUCCESS)
      {
        release();
        return false;
      }
    }
    return true;
  }

  void release() override
  {
    if(m_dvpCtx)
    {
      for(auto& slot : m_slots)
      {
        if(slot.dvpBuf)
        {
          nv_dvp_unregister(m_dvpCtx, slot.dvpBuf);
          slot.dvpBuf = nullptr;
        }
        if(slot.dmaLocked)
        {
          ajaDmaUnlock(m_card, slot.sysmem, m_sysmemBytes);
          slot.dmaLocked = false;
        }
        if(slot.sysmem)
        {
          nv_dvp_aligned_free(slot.sysmem);
          slot.sysmem = nullptr;
        }
      }
      if(m_dvpTex)
      {
        nv_dvp_unregister(m_dvpCtx, m_dvpTex);
        m_dvpTex = nullptr;
      }
      if(m_threadStarted)
      {
        nv_dvp_thread_end(m_dvpCtx);
        m_threadStarted = false;
      }
      nv_dvp_shutdown(m_dvpCtx);
      m_dvpCtx = nullptr;
    }
    // Don't delete cfg.outputTexture — we don't own it.
    m_glCtx = nullptr;
  }

  std::size_t slotCount() const noexcept override { return kSlotCount; }
  void* slotBuffer(std::size_t i) const noexcept override
  {
    return i < kSlotCount ? m_slots[i].sysmem : nullptr;
  }

  bool ingestFrame(std::size_t i) override
  {
    if(i >= kSlotCount)
      return false;
    auto& slot = m_slots[i];
    if(!slot.dvpBuf || !m_dvpTex)
      return false;
    if(nv_dvp_copy_buffer_to_texture(m_dvpCtx, slot.dvpBuf, m_dvpTex)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP-IN(GL): copy_buffer_to_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return false;
    }
    return true;
  }

  QRhiTexture* outputTexture() const noexcept override { return cfg.outputTexture; }

  void acquireForRender() override
  {
    if(m_dvpCtx && m_dvpTex)
      nv_dvp_acquire_texture(m_dvpCtx, m_dvpTex);
  }

  void releaseAfterRender() override
  {
    if(m_dvpCtx && m_dvpTex)
      nv_dvp_release_texture(m_dvpCtx, m_dvpTex);
  }
};

} // namespace Gfx::AJA
