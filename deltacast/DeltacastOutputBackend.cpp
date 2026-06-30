#include <deltacast/DeltacastOutputBackend.hpp>

#include <deltacast/DeltacastFormats.hpp>

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Deltacast
{

DeltacastOutputBackend::DeltacastOutputBackend(DeltacastOutputSettings settings)
    : m_settings{settings}
{
}

DeltacastOutputBackend::~DeltacastOutputBackend()
{
  close();
}

bool DeltacastOutputBackend::open(score::gfx::GraphicsApi)
{
  if(VHD_OpenBoardHandle(
         static_cast<ULONG>(m_settings.deviceIndex), &m_board, nullptr, 0)
         != VHDERR_NOERROR
     || !m_board)
    return false;

  // Genlock on the local 1/1 (European) clock; fractional rates would flip this
  // to VHD_CLOCKDIV_FRAC — a refinement.
  VHD_SetBoardProperty(m_board, VHD_SDI_BP_GENLOCK_CLOCK_DIV, VHD_CLOCKDIV_1);

  if(VHD_OpenStreamHandle(
         m_board, VHD_ST_TX0, VHD_SDI_STPROC_DISJOINED_VIDEO, nullptr, &m_stream,
         nullptr)
         != VHDERR_NOERROR
     || !m_stream)
  {
    close();
    return false;
  }

  const auto vstd = static_cast<VHD_VIDEOSTANDARD>(m_settings.videoStandard);
  const auto pack = static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking);

  ULONG w = 0, h = 0, fr = 0;
  BOOL32 interlaced = FALSE;
  VHD_GetVideoCharacteristics(vstd, &w, &h, &interlaced, &fr);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  m_frameRate = fr ? static_cast<double>(fr) : 60.0;
  m_rowBytes = static_cast<int>(vhdBytesPerLine(pack, m_width));
  m_frameByteSize
      = static_cast<uint32_t>(m_rowBytes) * static_cast<uint32_t>(m_height);

  VHD_SetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, m_settings.videoStandard);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFERQUEUE_DEPTH, 4);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFERQUEUE_PRELOAD, 2);
  VHD_SetStreamProperty(
      m_stream, VHD_SDI_SP_INTERFACE, vhdInterfaceFromStandard(vstd));
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFER_PACKING, m_settings.bufferPacking);

  if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
  {
    qWarning() << "Deltacast: VHD_StartStream (TX) failed";
    close();
    return false;
  }
  m_started = true;
  m_open = true;
  return true;
}

void DeltacastOutputBackend::close()
{
  if(m_stream)
  {
    if(m_started)
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
  m_open = false;
}

score::gfx::interop::VideoPixelFormat
DeltacastOutputBackend::wireFormat() const noexcept
{
  return neutralFromPacking(
      static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking));
}

bool DeltacastOutputBackend::prefersFloatRender() const noexcept
{
  return m_settings.bufferPacking == VHD_BUFPACK_VIDEO_YUV422_10;
}

QString DeltacastOutputBackend::colorConversion() const
{
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
}

std::vector<score::gfx::interop::HostStagedPlane>
DeltacastOutputBackend::planes() const
{
  return {{m_rowBytes, m_frameByteSize}}; // VHD wire packings are single-plane
}

score::gfx::interop::VendorDmaRegistrar DeltacastOutputBackend::registrar()
{
  // Host-staged v1: the SDK owns its slot buffers; HostStagedOutput's ring is
  // plain host memory that submitFrame() copies into a VHD slot. No page-lock.
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [](void*, std::uint32_t) { return true; };
  reg.releaseSlot = [](void*, std::uint32_t) {};
  return reg;
}

score::gfx::interop::PacedFramePump::Hooks DeltacastOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [] { return true; };
  h.canAccept = {}; // back-pressure is VHD_UnlockSlotHandle blocking in submit
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool DeltacastOutputBackend::submitFrame(void* framePtr)
{
  if(!m_stream || !framePtr)
    return false;

  HANDLE slot = nullptr;
  if(VHD_LockSlotHandle(m_stream, &slot) != VHDERR_NOERROR || !slot)
    return false;

  BYTE* buf = nullptr;
  ULONG size = 0;
  if(VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buf, &size) == VHDERR_NOERROR
     && buf)
  {
    std::memcpy(
        buf, framePtr,
        std::min<ULONG>(size, static_cast<ULONG>(m_frameByteSize)));
  }
  // Blocks until the board consumes the slot -> paces the pump to genlock.
  VHD_UnlockSlotHandle(slot);
  return true;
}

} // namespace Gfx::Deltacast
