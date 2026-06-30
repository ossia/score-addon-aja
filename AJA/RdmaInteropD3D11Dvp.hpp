#pragma once
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>
#include <Gfx/Graph/encoders/BGRA.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>
#include <Gfx/Graph/encoders/V210.hpp>
#include <nv_dvp_bridge.h>

#include <ntv2card.h>
#include <ntv2enums.h>
#include <ntv2formatdescriptor.h>

#include <QtGui/private/qrhid3d11_p.h>

#include <QDebug>

#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace Gfx::AJA
{

/**
 * @brief D3D11 strategy using NVIDIA "GPUDirect for Video" (DVP).
 *
 * Pipeline per frame:
 *   1. Input pipeline renders RGBA into cfg.sourceTexture (QRhi).
 *   2. Fragment encoder (V210/UYVY/BGRA) reads sourceTexture and writes
 *      AJA-format bytes into its own RGBA8 m_outTexture (QRhi-managed).
 *   3. After endOffscreenFrame, the bridge does a hardware DMA from
 *      m_outTexture -> a page-locked, AJA-DMA-locked sysmem buffer via
 *      `dvpMemcpyLined`. Synchronous (CPU waits for DMA completion).
 *   4. AJA's AutoCirculateTransfer ships the sysmem buffer over PCIe to
 *      the SDI card. (This step happens on AJANode's consumer thread.)
 *
 * vs. the existing encoder + CPU staging fallback: we replace QRhi's
 * `readBackTexture` (which goes texture -> CPU via the GPU readback
 * staging path, with a host stall) with a DVP-managed PCIe DMA that
 * runs on a separate engine and signals via hardware semaphores. AJA's
 * `dvplowlatencydemo` is the reference for this pattern.
 *
 * The fragment encoders' `m_outTexture` is the *encoded-format* texture
 * (e.g. (width/6)*4 x height for v210). DVP DMAs the texture's raw bytes
 * straight to the AJA buffer; the layout already matches the AJA frame
 * buffer format (encoders are designed that way).
 *
 * Lives next to the (Linux-only) tier-3 RDMA strategies under the same
 * RdmaInterop interface so AJANode's strategy-selection logic can pick
 * one or the other based on platform + bridge availability.
 */
struct RdmaInteropD3D11Dvp final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropD3D11Dvp(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  score::gfx::interop::GpuDirectStrategyConfig cfg{};
  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};

  ID3D11Device* m_dev{};

  NvDvpContextHandle m_dvpCtx{};
  NvDvpResourceHandle m_dvpTex{};
  NvDvpResourceHandle m_dvpBuf{};
  bool m_threadStarted{};

  std::unique_ptr<score::gfx::GPUVideoEncoder> m_encoder;
  QRhiTexture* m_encoderOutput{}; /* non-owning; encoder owns it */

  /* Page-locked AJA sysmem buffer. Allocated with nv_dvp_aligned_alloc to
   * guarantee 4 KB alignment that DVP needs; AJA `DMABufferLock` page-
   * locks the same memory for its own DMA path. */
  void* m_sysmem{};
  uint32_t m_sysmemBytes{};
  uint32_t m_sysmemStrideBytes{};
  uint32_t m_sysmemRows{};
  bool m_dmaLocked{};

  const char* name() const noexcept override { return "DVP-D3D11"; }

  static bool isSupported(QRhi* rhi)
  {
    /* No QRhi::Compute requirement: this path uses fragment encoders
     * which work everywhere QRhi works. The only encoder-side limit is
     * the v210 width%6==0 rule already enforced in AJANode's setup. */
    return rhi != nullptr;
  }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    cfg = c;
    if(!isSupported(cfg.rhi) || !cfg.state)
      return false;

    auto* native
        = static_cast<const QRhiD3D11NativeHandles*>(cfg.rhi->nativeHandles());
    if(!native || !native->dev)
      return false;
    m_dev = static_cast<ID3D11Device*>(native->dev);

    qDebug() << "AJA DVP(D3D11): loading dvp.dll...";
    if(nv_dvp_init_d3d11(&m_dvpCtx, m_dev) != NV_DVP_SUCCESS || !m_dvpCtx)
    {
      qWarning() << "AJA DVP(D3D11): init failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return false;
    }
    qDebug() << "AJA DVP(D3D11): dvp.dll loaded + D3D11 device bound";

    if(nv_dvp_thread_begin(m_dvpCtx) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): thread_begin failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      nv_dvp_shutdown(m_dvpCtx);
      m_dvpCtx = nullptr;
      return false;
    }
    m_threadStarted = true;

    /* Build the fragment encoder. We keep m_encoderOutput around to
     * register with DVP. */
    auto colorShader = score::gfx::colorMatrixOut(
        AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);

    m_encoder = makeFragmentEncoder(m_targetFormat);
    if(!m_encoder)
    {
      qWarning() << "AJA DVP(D3D11): no fragment encoder for format"
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
      qWarning() << "AJA DVP(D3D11): encoder did not produce an output texture";
      release();
      return false;
    }

    /* Compute encoder output texture dimensions (in RGBA8 texels) and
     * the stride/size in bytes. The AJA frame buffer layout matches
     * exactly (the encoders are designed that way). */
    const QSize texSize = m_encoderOutput->pixelSize();
    m_sysmemRows = uint32_t(texSize.height());
    m_sysmemStrideBytes = uint32_t(texSize.width()) * 4u;
    m_sysmemBytes = m_sysmemStrideBytes * m_sysmemRows;

    if(m_sysmemBytes != cfg.frameByteSize)
    {
      qWarning() << "AJA DVP(D3D11): encoder output size" << m_sysmemBytes
                 << "does not match AJA frame size" << cfg.frameByteSize
                 << "- aborting";
      release();
      return false;
    }

    /* Aligned allocation. 4 KB alignment is more than enough for DVP's
     * `bufferAddrAlignment` on every NVIDIA driver in the wild and is
     * what AJA's own demos use. */
    m_sysmem = nv_dvp_aligned_alloc(m_sysmemBytes);
    if(!m_sysmem)
    {
      qWarning() << "AJA DVP(D3D11): nv_dvp_aligned_alloc(" << m_sysmemBytes
                 << ") failed";
      release();
      return false;
    }
    std::memset(m_sysmem, 0, m_sysmemBytes);

    /* Page-lock for AJA's DMA path. Without this AJA's kernel driver
     * would re-lock the pages on every AutoCirculateTransfer. */
    if(!m_card->DMABufferLock(
           reinterpret_cast<const ULWord*>(m_sysmem),
           static_cast<ULWord>(m_sysmemBytes), /*inMap=*/true,
           /*inRDMA=*/false))
    {
      qWarning() << "AJA DVP(D3D11): DMABufferLock(sysmem) failed";
      release();
      return false;
    }
    m_dmaLocked = true;

    /* Register encoder output texture with DVP. We need the underlying
     * ID3D11Texture2D; QRhiTexture::nativeTexture() returns it. */
    auto nt = m_encoderOutput->nativeTexture();
    if(!nt.object)
    {
      qWarning() << "AJA DVP(D3D11): encoder texture has no native handle";
      release();
      return false;
    }
    auto* d3d11Tex = reinterpret_cast<ID3D11Texture2D*>(nt.object);

    if(nv_dvp_register_d3d11_texture(
           m_dvpCtx, d3d11Tex, NV_DVP_FORMAT_RGBA8,
           uint32_t(texSize.width()), uint32_t(texSize.height()), &m_dvpTex)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): register_d3d11_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      release();
      return false;
    }

    if(nv_dvp_register_sysmem_buffer(
           m_dvpCtx, m_sysmem, NV_DVP_FORMAT_RGBA8, uint32_t(texSize.width()),
           uint32_t(texSize.height()), m_sysmemStrideBytes, &m_dvpBuf)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): register_sysmem_buffer failed:"
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
    m_dev = nullptr;
  }

  void encodeFrame(QRhiCommandBuffer& cb) override
  {
    /* AJA's reference demo (oglapp.cpp main loop) brackets API
     * rendering with AcquireTexture (WaitAPI) BEFORE the render and
     * ReleaseTexture (EndAPI) AFTER. WaitAPI here transitions the
     * texture from DVP-side to API-side ownership; on the very first
     * frame there is no preceding EndDVP signal, but DVP handles that
     * case (the texture starts in a state where the first WaitAPI
     * passes immediately - same as in AJA's demo). */
    if(nv_dvp_acquire_texture(m_dvpCtx, m_dvpTex) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): acquire_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
    }
    /* Inside the offscreen frame: the encoder's exec() schedules the
     * fragment pass writing the encoded bytes to m_encoderOutput. It
     * also queues a readback into m_encoder->readback().data, which we
     * ignore - small wasted PCIe bandwidth, no other side effect. */
    m_encoder->exec(*cfg.rhi, cb);
  }

  void* prepareNextFrame() override
  {
    /* After endOffscreenFrame the encoder's writes are flushed to the
     * D3D11 immediate context. EndAPI inserts a signal at this point
     * in the API queue; DVP's WaitDVP inside the next call waits for
     * that signal before DMAing. */
    if(nv_dvp_release_texture(m_dvpCtx, m_dvpTex) != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): release_texture failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return nullptr;
    }
    if(nv_dvp_copy_texture_to_buffer(m_dvpCtx, m_dvpTex, m_dvpBuf)
       != NV_DVP_SUCCESS)
    {
      qWarning() << "AJA DVP(D3D11): copy_texture_to_buffer failed:"
                 << nv_dvp_get_error_string(m_dvpCtx);
      return nullptr;
    }

    /* Return the AJA-locked sysmem pointer to AJANode, which hands it
     * to AJAConsumerThread. The next frame's encodeFrame will call
     * acquire_texture, which waits for the EndDVP signal that just
     * fired inside copy_texture_to_buffer above. */
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

  /* The fragment encoders all expose `m_outTexture` as a public field.
   * Rather than touch the encoder base class to add a virtual getter,
   * we cast to the concrete type per format. */
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
