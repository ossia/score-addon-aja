#pragma once
#include <deltacast/Deltacast.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace score::gfx::interop
{
struct GpuDirectCaptureSlotRing;
}

namespace Gfx::Deltacast
{

struct DeltacastInputSettings
{
  int deviceIndex{0};
  ULONG videoStandard{0}; ///< 0 = auto-detect the incoming standard
  ULONG bufferPacking{VHD_BUFPACK_VIDEO_YUV422_8};
};

/**
 * @brief DELTACAST capture backend (score::gfx::DMACaptureBackend).
 *
 * Host-staged v1: a reception thread runs the VHD slot loop
 * (VHD_LockSlotHandle -> VHD_GetSlotBuffer -> memcpy into the capture
 * strategy's next slot -> VHD_UnlockSlotHandle) and publishes the slot in the
 * node's ring. The render thread uploads the slot via DeltacastCpuCapture.
 * RDMA GPU-direct is a later pass.
 */
class DeltacastInputBackend final : public score::gfx::DMACaptureBackend
{
public:
  DeltacastInputBackend(
      DeltacastInputSettings settings,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring);
  ~DeltacastInputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  pickStrategy(QRhi::Implementation) override
  {
    return {}; // host-staged only for now
  }
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::GpuDirectCaptureStrategy* s) noexcept override
  {
    m_strategy = s;
  }
  void start() override;
  void stop() override;

private:
  void runLoop();

  DeltacastInputSettings m_settings;
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;

  HANDLE m_board{nullptr};
  HANDLE m_stream{nullptr};
  score::gfx::interop::GpuDirectCaptureStrategy* m_strategy{};

  std::thread m_thread;
  std::atomic<bool> m_running{false};

  int m_width{};
  int m_height{};
  uint32_t m_frameByteSize{};
  bool m_started{};
};

} // namespace Gfx::Deltacast
