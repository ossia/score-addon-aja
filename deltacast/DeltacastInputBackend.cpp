#include <deltacast/DeltacastInputBackend.hpp>

#include <deltacast/DeltacastCpuCapture.hpp>
#include <deltacast/DeltacastFormats.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <Video/VideoInterface.hpp>

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Deltacast
{

DeltacastInputBackend::DeltacastInputBackend(
    DeltacastInputSettings settings,
    score::gfx::interop::GpuDirectCaptureSlotRing& ring)
    : m_settings{settings}, m_ring{ring}
{
}

DeltacastInputBackend::~DeltacastInputBackend()
{
  stop();
}

bool DeltacastInputBackend::open()
{
  if(VHD_OpenBoardHandle(
         static_cast<ULONG>(m_settings.deviceIndex), &m_board, nullptr, 0)
         != VHDERR_NOERROR
     || !m_board)
    return false;

  if(VHD_OpenStreamHandle(
         m_board, VHD_ST_RX0, VHD_SDI_STPROC_DISJOINED_VIDEO, nullptr, &m_stream,
         nullptr)
         != VHDERR_NOERROR
     || !m_stream)
  {
    if(m_board) { VHD_CloseBoardHandle(m_board); m_board = nullptr; }
    return false;
  }

  // Auto-detect the incoming standard unless one was pinned in the settings.
  ULONG vstd = m_settings.videoStandard;
  if(vstd == 0)
  {
    if(VHD_GetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, &vstd)
           != VHDERR_NOERROR
       || vstd == NB_VHD_VIDEOSTANDARDS)
    {
      qWarning() << "Deltacast input: no incoming video standard detected";
      VHD_CloseStreamHandle(m_stream); m_stream = nullptr;
      VHD_CloseBoardHandle(m_board); m_board = nullptr;
      return false;
    }
  }
  m_settings.videoStandard = vstd;

  const auto pack = static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking);
  ULONG w = 0, h = 0, fr = 0;
  BOOL32 interlaced = FALSE;
  VHD_GetVideoCharacteristics(
      static_cast<VHD_VIDEOSTANDARD>(vstd), &w, &h, &interlaced, &fr);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  m_frameByteSize = vhdBytesPerLine(pack, m_width) * static_cast<uint32_t>(m_height);

  VHD_SetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, vstd);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_TRANSFER_SCHEME, VHD_TRANSFER_SLAVED);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFER_PACKING, m_settings.bufferPacking);

  if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
  {
    qWarning() << "Deltacast input: VHD_StartStream (RX) failed";
    VHD_CloseStreamHandle(m_stream); m_stream = nullptr;
    VHD_CloseBoardHandle(m_board); m_board = nullptr;
    return false;
  }
  return true;
}

Video::ImageFormat DeltacastInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format = score::gfx::interop::toAVPixelFormat(
      neutralFromPacking(static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking)));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
DeltacastInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(
      neutralFromPacking(static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking)),
      meta);
}

std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
DeltacastInputBackend::makeCpuStrategy()
{
  return std::make_unique<DeltacastCpuCapture>();
}

void DeltacastInputBackend::start()
{
  if(m_started || !m_stream)
    return;
  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] { runLoop(); }};
  m_started = true;
}

void DeltacastInputBackend::stop()
{
  m_running.store(false, std::memory_order_release);
  if(m_thread.joinable())
    m_thread.join();
  if(m_stream)
  {
    VHD_StopStream(m_stream);
    VHD_CloseStreamHandle(m_stream);
    m_stream = nullptr;
  }
  if(m_board)
  {
    VHD_CloseBoardHandle(m_board);
    m_board = nullptr;
  }
  m_started = false;
}

void DeltacastInputBackend::runLoop()
{
  std::size_t writeIdx = 0;
  while(m_running.load(std::memory_order_acquire))
  {
    auto* strat = m_strategy;
    if(!strat)
      continue;
    const std::size_t slots = strat->slotCount();
    if(slots == 0)
      continue;

    HANDLE slot = nullptr;
    const ULONG r = VHD_LockSlotHandle(m_stream, &slot);
    if(r != VHDERR_NOERROR)
    {
      if(r == VHDERR_TIMEOUT)
        continue; // no signal yet; keep polling
      break;
    }

    BYTE* buf = nullptr;
    ULONG size = 0;
    if(VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buf, &size) == VHDERR_NOERROR
       && buf)
    {
      if(void* dst = strat->slotBuffer(writeIdx))
      {
        std::memcpy(
            dst, buf,
            std::min<ULONG>(size, static_cast<ULONG>(m_frameByteSize)));
        strat->ingestFrame(writeIdx);
        m_ring.latestSlot.store(writeIdx, std::memory_order_release);
        m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
        writeIdx = (writeIdx + 1) % slots;
      }
    }
    VHD_UnlockSlotHandle(slot);
  }
}

} // namespace Gfx::Deltacast
