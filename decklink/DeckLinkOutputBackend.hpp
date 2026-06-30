#pragma once
#include <decklink/DeckLink.hpp>
#include <decklink/DeckLinkComPtr.hpp>

#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <cstdint>
#include <semaphore>
#include <unordered_map>

namespace Gfx::DeckLink
{

struct DeckLinkOutputSettings
{
  int deviceIndex{0};
  BMDDisplayMode displayMode{bmdModeHD1080p5994};
  BMDPixelFormat pixelFormat{bmdFormat8BitYUV};
};

/**
 * @brief DeckLink playout backend (score::gfx::DirectVideoOutputBackend).
 *
 * Scheduled playback (the canonical SignalGenerator-sample model): host frames
 * from HostStagedOutput's pinned ring are wrapped zero-copy as IDeckLinkVideoFrames
 * (registrar -> IDeckLinkVideoBuffer + CreateVideoFrameWithBuffer) and handed to
 * ScheduleVideoFrame. Pacing bridges DeckLink's push-model completion callback to
 * the pump's pull-model via a counting semaphore (a permit = a free output slot;
 * seeded with the preroll, released on each ScheduledFrameCompleted).
 */
class DeckLinkOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit DeckLinkOutputBackend(DeckLinkOutputSettings settings);
  ~DeckLinkOutputBackend() override;

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
    // No single-buffer GPU-direct strategy: DeckLink's async scheduled playback
    // needs a frame ring, which the host-staged path provides. Output DVP is
    // engaged via prefersGpuDownload() -> HostStagedOutput's HostPinnedRing.
    return {};
  }
  /// Opt into the GPU-direct (DVP) download in HostStagedOutput: the encoder
  /// texture is DMA'd straight into the pinned ring slots (each wrapped as a
  /// DeckLink frame via registrar()), skipping the QRhi readback. Falls back to
  /// CPU readback when no DVP backend is present.
  bool prefersGpuDownload() const noexcept override;
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

  /// Released by the completion callback (one permit per freed output slot).
  std::counting_semaphore<64>& freeSlots() noexcept { return m_freeSlots; }

private:
  bool waitForTick();
  bool submitFrame(void* framePtr);

  DeckLinkOutputSettings m_settings;

  ComPtr<IDeckLink> m_device;
  ComPtr<IDeckLinkOutput> m_output;
  ComPtr<IDeckLinkVideoOutputCallback> m_callback;
  // Pinned host slot -> the DeckLink frame wrapping it (zero-copy, session-lived).
  std::unordered_map<void*, ComPtr<IDeckLinkMutableVideoFrame>> m_frames;

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{};
  double m_frameRate{60.0};
  BMDTimeValue m_frameDuration{1001};
  BMDTimeScale m_timeScale{60000};

  std::uint64_t m_frameCount{0}; ///< scheduled-frame display-time counter
  int m_scheduled{0};
  bool m_started{false};
  bool m_open{false};

  static constexpr int kPreroll = 3;
  std::counting_semaphore<64> m_freeSlots{0};
};

} // namespace Gfx::DeckLink
