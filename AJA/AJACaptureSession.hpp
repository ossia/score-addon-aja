#pragma once
#include <AJA/AJAInput.hpp>

#include <Video/VideoInterface.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ntv2enums.h>
#include <ntv2publicinterface.h>

class CNTV2Card;

namespace Gfx::AJA
{

/**
 * @brief Shared AJA SDI input setup + per-frame metadata extraction.
 *
 * Owns a CNTV2Card, configures it for input on a master channel +
 * resolution/routing topology, drives signal-change detection, and
 * extracts per-frame VPID-derived color metadata, RP188 timecode,
 * SDI link status, and ST 2108-1 HDR static metadata when the SDI
 * source carries them.
 *
 * Used by composition by both `aja_input_capture` (CPU-staging
 * AVFrame producer) and `aja_gpu_capture` (GPU-direct CaptureInterop
 * producer): they call open() + setupChannel() + AutoCirculateInit /
 * Start / Stop themselves, then run their own per-VBI loop, calling
 * the session's helpers for ANC buffer plumbing and per-frame
 * metadata extraction.
 *
 * Lifecycle:
 *   1. open() - acquire card, IsDeviceReady, save task mode, OEM mode,
 *      free-run reference, VBI subscriptions.
 *   2. setupChannel() - probe input format + promote to 4K/8K if
 *      requested, pick frame-store / SDI counts (12G single-link vs
 *      quad SQD/TSI), enable channels, set FBF, route. Reads VPID +
 *      sets m_imageFormat. Allocates ANC buffers.
 *   3. detectAndApplyFormatChange() - call from runLoop every ~30 VBIs;
 *      reroutes if signal changed.
 *   4. attachAncToTransfer() / readPerFrameMetadata() - per-VBI helpers.
 *   5. teardownChannel() / close() - reverse setup.
 *
 * The capture loop (per-VBI work) is NOT owned by this class. Each
 * subclass handles its own AutoCirculate ring + buffer policy.
 */
class CaptureSession
{
public:
  /// Suggested AutoCirculate option flags. Owners OR these into
  /// the options arg of AutoCirculateInitForInput.
  static constexpr uint32_t kSuggestedAcOptions
      = AUTOCIRCULATE_WITH_RP188 | AUTOCIRCULATE_WITH_ANC;

  explicit CaptureSession(const AJAInputSettings& settings);
  ~CaptureSession();

  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;

  /**
   * @brief Open the AJA card, acquire it, and put it in OEM-tasks
   *        mode so the retail-services daemon doesn't fight us.
   *        Subscribes VBI events and sets the reference to free-run.
   *
   * Returns false on any failure; the session is left closed.
   */
  bool open();

  /// Reverse of open(). Restores task mode, drops VBI subscriptions,
  /// unlocks all DMA buffers, releases the stream.
  void close();

  /**
   * @brief Configure the channel for the currently-detected input
   *        format. Idempotent: also called by detectAndApplyFormatChange
   *        to reroute on signal change.
   *
   * Reads VPID after the route stabilizes; populates m_imageFormat
   * (color_space, color_primaries, color_trc, color_range) so any
   * downstream colorMatrix() lookup picks the correct path.
   *
   * Allocates ANC F1/F2 buffers based on the size reported by
   * `kVRegAncField1Offset` / `kVRegAncField2Offset` virtual registers.
   */
  bool setupChannel();

  /// Tear down the channel: AC stop, drop QuadQuad / 4K mode bits,
  /// drop transient routing state. Doesn't release the device.
  void teardownChannel();

  /**
   * @brief If the input format changed (cable swap / camera mode
   *        change), reroute and reinit. Returns true if a change was
   *        applied — callers should then skip one VBI before
   *        AutoCirculate produces frames again.
   */
  bool detectAndApplyFormatChange();

  /// Pre-transfer hook: hand the AC transfer the F1/F2 ANC buffers.
  /// Caller invokes on every loop iteration before AutoCirculateTransfer.
  /// No-op when ANC buffers haven't been allocated.
  void attachAncToTransfer(AUTOCIRCULATE_TRANSFER& xfer) noexcept;

  /**
   * @brief Post-transfer hook: extract timecode, ANC payloads (HDR
   *        static metadata), update SDI link status periodically.
   *        Caller invokes on every loop iteration after a successful
   *        AutoCirculateTransfer.
   */
  void readPerFrameMetadata(const AUTOCIRCULATE_TRANSFER& xfer);

  // ──────────────────────────────────────────────────────────────
  // Read accessors (set during setupChannel)
  // ──────────────────────────────────────────────────────────────

  CNTV2Card* card() const noexcept { return m_card.get(); }
  const AJAInputSettings& settings() const noexcept { return m_settings; }
  NTV2Channel masterChannel() const noexcept { return m_masterChannel; }
  NTV2VideoFormat videoFormat() const noexcept { return m_videoFormat; }
  NTV2FrameBufferFormat bufferFormat() const noexcept { return m_bufferFormat; }
  int width() const noexcept { return m_width; }
  int height() const noexcept { return m_height; }
  uint32_t frameSize() const noexcept { return m_frameSize; }
  double fps() const noexcept { return m_fps; }
  /// Image format including VPID-derived color metadata + any HDR10
  /// static metadata that has been parsed out of the most recent
  /// frame's ANC. Updated under m_imageFormatMutex.
  Video::ImageFormat imageFormat() const;
  /// AVPixelFormat for AVFrame consumers (CPU-staging path).
  AVPixelFormat avPixelFormat() const noexcept;

  // ──────────────────────────────────────────────────────────────
  // Per-frame status surfaced as atomics for parameter exposure.
  // ──────────────────────────────────────────────────────────────

  /// True when the SDI link is locked. Re-checked once per VBI poll
  /// period (~30 VBIs); stale by up to half a second otherwise.
  std::atomic<bool> signalLocked{false};
  /// Cumulative CRC error count from `ReadSDIStatistics`. Monotonic;
  /// readers can compute per-second rates by sampling on a timer.
  std::atomic<uint32_t> crcErrorCount{0};

  /// Last formatted RP188 timecode "HH:MM:SS:FF". Empty if no valid
  /// TC was attached to the most recent frame. Read with the
  /// timecode mutex held — short critical section, just a string copy.
  mutable std::mutex timecodeMutex;
  std::string lastTimecode;

private:
  bool routeSignal(
      UWord fbCount, UWord sdiCount, bool useTSI, bool isQuadQuad, bool isQQHFR);
  void readVPID();
  void readSDIStatus();
  void readTimecodes(const AUTOCIRCULATE_TRANSFER& xfer);
  void readAncPayloads(const AUTOCIRCULATE_TRANSFER& xfer);

  AJAInputSettings m_settings;

  // Synchronizes m_imageFormat (writable from the capture thread when
  // ANC parsing extracts new HDR static metadata) so the renderer can
  // pull a consistent snapshot for color-matrix decisions.
  mutable std::mutex m_imageFormatMutex;
  Video::ImageFormat m_imageFormat;

  std::unique_ptr<CNTV2Card> m_card;

  /// Saved retail-services task mode; restored on close() so we don't
  /// leave the card stuck in OEM mode for a follow-on application.
  NTV2EveryFrameTaskMode m_savedTaskMode{NTV2_STANDARD_TASKS};
  bool m_taskModeSaved{false};

  NTV2Channel m_masterChannel{NTV2_CHANNEL1};
  NTV2ChannelSet m_activeFrameStores;
  NTV2ChannelSet m_activeSDIs;
  NTV2VideoFormat m_videoFormat{NTV2_FORMAT_UNKNOWN};
  NTV2FrameBufferFormat m_bufferFormat{NTV2_FBF_8BIT_YCBCR};
  bool m_isQuadQuad{false};
  bool m_is4K{false};
  bool m_useTSI{false};
  /// True when the active route is 12G single-link (one SDI carries
  /// the whole 4K/8K signal). Picked when `features().CanDo12gRouting()`
  /// is true and the detected format is already a 4K/8K format —
  /// promotion-from-HD/4K paths still use quad-link.
  bool m_use12G{false};

  /// Page-locked F1/F2 ANC buffers for ST 2108 HDR static metadata,
  /// timecode VITC, captions, AFD, etc. Sized from the
  /// `kVRegAncField1Offset` / `kVRegAncField2Offset` virtual
  /// registers at setupChannel time.
  std::vector<uint8_t> m_ancF1Buf;
  std::vector<uint8_t> m_ancF2Buf;
  uint32_t m_ancF1Size{0};
  uint32_t m_ancF2Size{0};

  int m_width{};
  int m_height{};
  uint32_t m_frameSize{};
  double m_fps{};

  /// Counter for spacing periodic status polls (SDI lock + CRC).
  /// Bumped on every readPerFrameMetadata() call; the SDI register
  /// read happens once per kStatusPollPeriod calls.
  uint32_t m_statusPollCounter{0};
};

} // namespace Gfx::AJA
