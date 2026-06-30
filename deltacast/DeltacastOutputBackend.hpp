#pragma once
#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace Gfx::Deltacast
{

struct DeltacastOutputSettings
{
  int deviceIndex{0};
  ULONG videoStandard{VHD_VIDEOSTD_S274M_1080p_60Hz}; ///< VHD_VIDEOSTANDARD
  ULONG bufferPacking{VHD_BUFPACK_VIDEO_YUV422_8};    ///< VHD_BUFFERPACKING
};

/**
 * @brief DELTACAST SDI playout backend (score::gfx::DirectVideoOutputBackend).
 *
 * Host-staged v1 using the SDK-owned slot model: HostStagedOutput renders +
 * reads back the wire-format frame into its pinned ring, then submitFrame()
 * copies it into a VHD slot (VHD_LockSlotHandle -> VHD_GetSlotBuffer ->
 * memcpy -> VHD_UnlockSlotHandle). VHD_UnlockSlotHandle blocks until the board
 * consumes the slot, which paces the pump to genlock. GPU-direct (RDMA) later.
 */
class DeltacastOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit DeltacastOutputBackend(DeltacastOutputSettings settings);
  ~DeltacastOutputBackend() override;

  bool open(score::gfx::GraphicsApi api) override;
  void close() override;

  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  double frameRate() const noexcept override { return m_frameRate; }
  bool isOpen() const noexcept override { return m_open; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  int visibleRows() const noexcept override { return m_height; }
  score::gfx::interop::VideoPixelFormat wireFormat() const noexcept override;
  bool prefersFloatRender() const noexcept override;
  QString colorConversion() const override;
  std::vector<score::gfx::interop::HostStagedPlane> planes() const override;
  score::gfx::interop::VendorDmaRegistrar registrar() override;
  std::vector<std::function<std::unique_ptr<score::gfx::interop::GpuDirectStrategy>()>>
  gpuDirectCandidates(QRhi*, score::gfx::GraphicsApi) override
  {
    return {}; // host-staged only for now; RDMA (Seam B) is a later pass
  }
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

private:
  bool submitFrame(void* framePtr);

  DeltacastOutputSettings m_settings;

  HANDLE m_board{nullptr};
  HANDLE m_stream{nullptr};

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{};
  double m_frameRate{60.0};
  bool m_open{false};
  bool m_started{false};
};

} // namespace Gfx::Deltacast
