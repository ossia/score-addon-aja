#include <decklink/DeckLinkOutputBackend.hpp>

#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkFormats.hpp>

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <atomic>
#include <chrono>

namespace Gfx::DeckLink
{
namespace
{

/// Zero-copy IDeckLinkVideoBuffer over a pinned host slot. Wrapped by
/// CreateVideoFrameWithBuffer so the card DMAs straight from HostStagedOutput's
/// ring (no extra copy).
class BufferFrame final : public IDeckLinkVideoBuffer
{
public:
  BufferFrame(void* p, ULONGLONG n) : m_p{p}, m_n{n} {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
  {
    if(!ppv)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDeckLinkVideoBuffer))
    {
      *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG r = --m_ref;
    if(r == 0)
      delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
  {
    *buffer = m_p;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSize(ULONGLONG* size) override
  {
    *size = m_n;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags) override { return S_OK; }

private:
  void* m_p{};
  ULONGLONG m_n{};
  std::atomic<ULONG> m_ref{1};
};

/// Scheduled-playback completion callback: each freed output slot releases one
/// semaphore permit, which the pump's waitForTick() consumes (push -> pull bridge).
class CompletionCallback final : public IDeckLinkVideoOutputCallback
{
public:
  explicit CompletionCallback(std::counting_semaphore<64>& sem) : m_sem{sem} {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
  {
    if(!ppv)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IUnknown)
       || IsEqualIID(iid, IID_IDeckLinkVideoOutputCallback))
    {
      *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG r = --m_ref;
    if(r == 0)
      delete this;
    return r;
  }

  HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
      IDeckLinkVideoFrame*, BMDOutputFrameCompletionResult) override
  {
    m_sem.release();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override { return S_OK; }

private:
  std::counting_semaphore<64>& m_sem;
  std::atomic<ULONG> m_ref{1};
};

} // namespace

DeckLinkOutputBackend::DeckLinkOutputBackend(DeckLinkOutputSettings settings)
    : m_settings{settings}
{
}

DeckLinkOutputBackend::~DeckLinkOutputBackend()
{
  close();
}

bool DeckLinkOutputBackend::open(score::gfx::GraphicsApi)
{
  ensureComInit();
  m_device = openDevice(m_settings.deviceIndex);
  if(!m_device)
    return false;
  if(m_device->QueryInterface(IID_IDeckLinkOutput, m_output.putVoid()) != S_OK
     || !m_output)
    return false;

  // Resolve the display mode -> geometry + frame rate.
  ComPtr<IDeckLinkDisplayMode> mode;
  ComPtr<IDeckLinkDisplayModeIterator> modeIt;
  if(m_output->GetDisplayModeIterator(modeIt.put()) == S_OK && modeIt)
  {
    ComPtr<IDeckLinkDisplayMode> m;
    while(modeIt->Next(m.put()) == S_OK)
    {
      if(m->GetDisplayMode() == m_settings.displayMode)
      {
        mode = m;
        break;
      }
      m.reset();
    }
  }
  if(!mode)
  {
    qWarning() << "DeckLink: display mode not supported";
    return false;
  }
  m_width = static_cast<int>(mode->GetWidth());
  m_height = static_cast<int>(mode->GetHeight());
  BMDTimeValue dur = 0;
  BMDTimeScale scale = 0;
  if(mode->GetFrameRate(&dur, &scale) == S_OK && dur > 0)
  {
    m_frameDuration = dur;
    m_timeScale = scale;
    m_frameRate = double(scale) / double(dur);
  }

  int rb = 0;
  m_output->RowBytesForPixelFormat(m_settings.pixelFormat, m_width, &rb);
  m_rowBytes = rb;
  m_frameByteSize = static_cast<uint32_t>(rb) * static_cast<uint32_t>(m_height);

  if(m_output->EnableVideoOutput(m_settings.displayMode, bmdVideoOutputFlagDefault)
     != S_OK)
  {
    qWarning() << "DeckLink: EnableVideoOutput failed";
    return false;
  }

  m_callback = ComPtr<IDeckLinkVideoOutputCallback>(
      new CompletionCallback(m_freeSlots)); // adopt the initial ref
  m_output->SetScheduledFrameCompletionCallback(m_callback.get());

  // Seed preroll permits so the first kPreroll submits fill the queue, then
  // StartScheduledPlayback fires (in submitFrame).
  m_freeSlots.release(kPreroll);
  m_open = true;
  return true;
}

void DeckLinkOutputBackend::close()
{
  if(m_output)
  {
    if(m_started)
    {
      BMDTimeValue actualStop = 0;
      m_output->StopScheduledPlayback(0, &actualStop, m_timeScale);
      m_started = false;
    }
    m_output->SetScheduledFrameCompletionCallback(nullptr);
    m_output->DisableVideoOutput();
  }
  m_frames.clear();
  m_callback.reset();
  m_output.reset();
  m_device.reset();
  m_open = false;
}

score::gfx::interop::VideoPixelFormat
DeckLinkOutputBackend::wireFormat() const noexcept
{
  return toNeutralFormat(m_settings.pixelFormat);
}

bool DeckLinkOutputBackend::prefersFloatRender() const noexcept
{
  switch(m_settings.pixelFormat)
  {
    case bmdFormat10BitYUV:
    case bmdFormat10BitRGB:
    case bmdFormat12BitRGB:
    case bmdFormat12BitRGBLE:
      return true;
    default:
      return false;
  }
}

QString DeckLinkOutputBackend::colorConversion() const
{
  // SDR BT.709 limited range. HDR (via IDeckLinkVideoFrameMutableMetadata
  // Extensions) is a later pass.
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
}

std::vector<score::gfx::interop::HostStagedPlane>
DeckLinkOutputBackend::planes() const
{
  return {{m_rowBytes, m_frameByteSize}}; // DeckLink wire formats are packed (1 plane)
}

score::gfx::interop::VendorDmaRegistrar DeckLinkOutputBackend::registrar()
{
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [this](void* ptr, std::uint32_t size) -> bool {
    if(m_frames.find(ptr) != m_frames.end())
      return true;
    auto* buf = new BufferFrame(ptr, size); // refcount 1
    ComPtr<IDeckLinkMutableVideoFrame> frame;
    const HRESULT hr = m_output->CreateVideoFrameWithBuffer(
        m_width, m_height, m_rowBytes, m_settings.pixelFormat,
        bmdFrameFlagDefault, buf, frame.put());
    buf->Release(); // the frame now owns the buffer ref
    if(FAILED(hr) || !frame)
      return false;
    m_frames.emplace(ptr, std::move(frame));
    return true;
  };
  reg.releaseSlot = [this](void* ptr, std::uint32_t) { m_frames.erase(ptr); };
  return reg;
}

bool DeckLinkOutputBackend::prefersGpuDownload() const noexcept
{
#if defined(SCORE_HAS_AJA_DVP_BRIDGE)
  return true; // nv_dvp_bridge linked: HostStagedOutput can DVP texture->sysmem
#else
  return false;
#endif
}

score::gfx::interop::PacedFramePump::Hooks DeckLinkOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [this] { return waitForTick(); };
  h.canAccept = {}; // back-pressure is the semaphore in waitForTick
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool DeckLinkOutputBackend::waitForTick()
{
  return m_freeSlots.try_acquire_for(std::chrono::milliseconds(100));
}

bool DeckLinkOutputBackend::submitFrame(void* framePtr)
{
  const auto it = m_frames.find(framePtr);
  if(it == m_frames.end())
    return false;

  const HRESULT hr = m_output->ScheduleVideoFrame(
      it->second.get(),
      static_cast<BMDTimeValue>(m_frameCount * m_frameDuration), m_frameDuration,
      m_timeScale);
  if(FAILED(hr))
    return false;

  ++m_frameCount;
  if(!m_started && ++m_scheduled >= kPreroll)
  {
    m_output->StartScheduledPlayback(0, m_timeScale, 1.0);
    m_started = true;
  }
  return true;
}

} // namespace Gfx::DeckLink
