#pragma once
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/encoders/BGRA.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>
#include <Gfx/Graph/encoders/V210.hpp>
#include <nv_dvp_bridge.h>

#include <ntv2card.h>
#include <ntv2enums.h>

#include <QtGui/private/qrhigles2_p.h>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <QDebug>

#include <cstdint>
#include <memory>

namespace Gfx::AJA
{

/**
 * @brief OpenGL strategy using NVIDIA "GPUDirect for Video" (DVP).
 *
 * Same architecture as RdmaInteropD3D11Dvp but using DVP's OpenGL
 * binding (`dvpInitGLContext` / `dvpCreateGPUTextureGL`). The QRhi
 * GL backend's context must be current on the calling thread when
 * init() runs and when transfer functions run; AJANode dispatches
 * encodeFrame / prepareNextFrame from its render thread which is the
 * thread QRhi made the GL context current on.
 */
struct RdmaInteropGLDvp final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropGLDvp(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  score::gfx::interop::GpuDirectStrategyConfig cfg{};
  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};

  QOpenGLContext* m_glCtx{};

  NvDvpContextHandle m_dvpCtx{};
  NvDvpResourceHandle m_dvpTex{};
  NvDvpResourceHandle m_dvpBuf{};
  bool m_threadStarted{};

  std::unique_ptr<score::gfx::GPUVideoEncoder> m_encoder;
  QRhiTexture* m_encoderOutput{};

  void* m_sysmem{};
  uint32_t m_sysmemBytes{};
  uint32_t m_sysmemStrideBytes{};
  uint32_t m_sysmemRows{};
  bool m_dmaLocked{};

  const char* name() const noexcept override { return "DVP-GL"; }

  static bool isSupported(QRhi* rhi) { return rhi != nullptr; }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    cfg = c;
    if(!isSupported(cfg.rhi) || !cfg.state)
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
      qWarning() << "AJA DVP(GL): no offscreen surface available to bind the GL "
                    "context; cannot init DVP";
      return false;
    }
    if(!m_glCtx->makeCurrent(cfg.state->surface))
    {
      qWarning() << "AJA DVP(GL): makeCurrent() failed; cannot bind DVP to the "
                    "GL context";
      return false;
    }

    qDebug() << "AJA DVP(GL): loading dvp.dll...";
    if(nv_dvp_init_gl(&m_dvpCtx) != NV_DVP_SUCCESS || !m_dvpCtx)
    {
      qWarning() << "AJA DVP(GL): init failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return false;
    }
    qDebug() << "AJA DVP(GL): dvp.dll loaded + GL context bound";
    if(nv_dvp_thread_begin(m_dvpCtx) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): thread_begin failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      nv_dvp_shutdown(m_dvpCtx);
      m_dvpCtx = nullptr;
      return false;
    }
    m_threadStarted = true;

    auto colorShader = score::gfx::colorMatrixOut(
        AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);

    m_encoder = makeFragmentEncoder(m_targetFormat);
    if(!m_encoder)
    {
      qWarning() << "AJA DVP(GL): no fragment encoder for format"
                 << int(m_targetFormat);
      release();
      return false;
    }
    m_encoder->init(
        *cfg.rhi, *cfg.state, cfg.sourceTexture, cfg.width, cfg.height,
        colorShader);
    m_encoderOutput = encoderOutputTexture(m_encoder.get(), m_targetFormat);
    if(!m_encoderOutput)
    {
      qWarning() << "AJA DVP(GL): encoder did not produce an output texture";
      release();
      return false;
    }

    const QSize texSize = m_encoderOutput->pixelSize();
    m_sysmemRows = uint32_t(texSize.height());
    m_sysmemStrideBytes = uint32_t(texSize.width()) * 4u;
    m_sysmemBytes = m_sysmemStrideBytes * m_sysmemRows;

    if(m_sysmemBytes != cfg.frameByteSize)
    {
      qWarning() << "AJA DVP(GL): encoder output size" << m_sysmemBytes
                 << "does not match AJA frame size" << cfg.frameByteSize;
      release();
      return false;
    }

    m_sysmem = nv_dvp_aligned_alloc(m_sysmemBytes);
    if(!m_sysmem)
    {
      qWarning() << "AJA DVP(GL): nv_dvp_aligned_alloc(" << m_sysmemBytes
                 << ") failed";
      release();
      return false;
    }
    std::memset(m_sysmem, 0, m_sysmemBytes);

    if(!m_card->DMABufferLock(
           reinterpret_cast<const ULWord*>(m_sysmem),
           static_cast<ULWord>(m_sysmemBytes), /*inMap=*/true,
           /*inRDMA=*/false))
    {
      qWarning() << "AJA DVP(GL): DMABufferLock(sysmem) failed";
      release();
      return false;
    }
    m_dmaLocked = true;

    auto nt = m_encoderOutput->nativeTexture();
    if(!nt.object)
    {
      qWarning() << "AJA DVP(GL): encoder texture has no native handle";
      release();
      return false;
    }
    const uint32_t glTexId = uint32_t(nt.object);

    if(nv_dvp_register_gl_texture(
           m_dvpCtx, glTexId, NV_DVP_FORMAT_RGBA8,
           uint32_t(texSize.width()), uint32_t(texSize.height()), &m_dvpTex)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): register_gl_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      release();
      return false;
    }
    if(nv_dvp_register_sysmem_buffer(
           m_dvpCtx, m_sysmem, NV_DVP_FORMAT_RGBA8, uint32_t(texSize.width()),
           uint32_t(texSize.height()), m_sysmemStrideBytes, &m_dvpBuf)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): register_sysmem_buffer failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      release();
      return false;
    }

    return true;
  }

  void release() override
  {
    if(m_dvpCtx)
    {
      if(m_dvpTex)
      {
        nv_dvp_unregister(m_dvpCtx, m_dvpTex);
        m_dvpTex = nullptr;
      }
      if(m_dvpBuf)
      {
        nv_dvp_unregister(m_dvpCtx, m_dvpBuf);
        m_dvpBuf = nullptr;
      }
      if(m_threadStarted)
      {
        nv_dvp_thread_end(m_dvpCtx);
        m_threadStarted = false;
      }
      nv_dvp_shutdown(m_dvpCtx);
      m_dvpCtx = nullptr;
    }
    if(m_encoder)
    {
      m_encoder->release();
      m_encoder.reset();
    }
    m_encoderOutput = nullptr;

    if(m_dmaLocked && m_card && m_sysmem)
    {
      m_card->DMABufferUnlock(
          reinterpret_cast<const ULWord*>(m_sysmem),
          static_cast<ULWord>(m_sysmemBytes));
      m_dmaLocked = false;
    }
    if(m_sysmem)
    {
      nv_dvp_aligned_free(m_sysmem);
      m_sysmem = nullptr;
    }
    m_glCtx = nullptr;
  }

  void encodeFrame(QRhiCommandBuffer& cb) override
  {
    /* AcquireTexture before render, ReleaseTexture after - matches
     * AJA's oglapp.cpp main loop. See RdmaInteropD3D11Dvp.hpp for the
     * design rationale. */
    if(nv_dvp_acquire_texture(m_dvpCtx, m_dvpTex) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): acquire_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
    }
    m_encoder->exec(*cfg.rhi, cb);
  }

  void* prepareNextFrame() override
  {
    if(nv_dvp_release_texture(m_dvpCtx, m_dvpTex) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): release_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return nullptr;
    }
    if(nv_dvp_copy_texture_to_buffer(m_dvpCtx, m_dvpTex, m_dvpBuf)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(GL): copy_texture_to_buffer failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return nullptr;
    }
    return m_sysmem;
  }

private:
  static std::unique_ptr<score::gfx::GPUVideoEncoder>
  makeFragmentEncoder(NTV2FrameBufferFormat fmt)
  {
    switch(fmt)
    {
      case NTV2_FBF_8BIT_YCBCR:
        return std::make_unique<score::gfx::UYVYEncoder>();
      case NTV2_FBF_10BIT_YCBCR:
        return std::make_unique<score::gfx::V210Encoder>();
      case NTV2_FBF_ARGB:
        return std::make_unique<score::gfx::BGRAEncoder>();
      default:
        return nullptr;
    }
  }

  static QRhiTexture* encoderOutputTexture(
      score::gfx::GPUVideoEncoder* enc, NTV2FrameBufferFormat fmt)
  {
    if(!enc)
      return nullptr;
    switch(fmt)
    {
      case NTV2_FBF_8BIT_YCBCR:
        return static_cast<score::gfx::UYVYEncoder*>(enc)->m_outTexture;
      case NTV2_FBF_10BIT_YCBCR:
        return static_cast<score::gfx::V210Encoder*>(enc)->m_outTexture;
      case NTV2_FBF_ARGB:
        return static_cast<score::gfx::BGRAEncoder*>(enc)->m_outTexture;
      default:
        return nullptr;
    }
  }
};

} // namespace Gfx::AJA
