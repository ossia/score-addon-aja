// Standalone (SDK-only, no score/Qt) reproducer for the ARGB-framebuffer ->
// SDI "black output" issue. Mirrors the canonical AJA ntv2outputtestpattern
// demo: card OUT plays out a test pattern from an RGB (or YCbCr) framebuffer
// via FrameStore -> CSC -> SDI, using DMAWriteFrame to the current output
// frame (NOT AutoCirculate). Card IN captures the SDI wire as 8-bit YCbCr and
// reports whether the result is black or carries the pattern.
//
//   ./aja_rgb_repro             # ARGB output (the failing case), card0->card1
//   ./aja_rgb_repro --yuv       # YCbCr8 output (control: should show bars)
//   ./aja_rgb_repro --ac        # ARGB output via AutoCirculate (our path)
//
// If ARGB(DMAWriteFrame) shows bars but our ossia AutoCirculate path is black,
// the bug is in the AutoCirculate ARGB setup. If ARGB is black here too, the
// card/firmware doesn't drive RGB-framebuffer playout on this setup.

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2formatdescriptor.h>
#include <ntv2signalrouter.h>
#include <ntv2testpatterngen.h>
#include <ntv2utils.h>

#include <ajabase/system/process.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static const uint32_t kSig = NTV2_FOURCC('r', 'e', 'p', 'r');

namespace
{
bool openCard(int idx, CNTV2Card& card)
{
  if(!CNTV2DeviceScanner::GetDeviceAtIndex(UWord(idx), card))
    return false;
  if(!card.IsDeviceReady(false))
    return false;
  card.AcquireStreamForApplication(kSig, int32_t(AJAProcess::GetPid()));
  card.SetEveryFrameServices(NTV2_OEM_TASKS);
  if(card.features().CanDoMultiFormat())
    card.SetMultiFormatMode(false);
  return true;
}

// Mirror of ntv2outputtestpattern::SetUpVideo + RouteOutputSignal + EmitPattern.
bool setupOutput(
    CNTV2Card& card, NTV2VideoFormat vf, NTV2FrameBufferFormat fbf, bool autoCirc)
{
  const NTV2Channel ch = NTV2_CHANNEL1;
  card.SetVideoFormat(vf, false, false, ch);
  card.SetReference(NTV2_REFERENCE_FREERUN);
  card.SetFrameBufferFormat(ch, fbf);
  card.EnableChannel(ch);
  card.SetMode(ch, NTV2_MODE_DISPLAY);
  if(card.features().HasBiDirectionalSDI())
    card.SetSDITransmitEnable(ch, true);

  // Route FrameStore -> [CSC if RGB] -> SDIOut1 (exact demo logic).
  NTV2XptConnections conns;
  const bool isRGB = ::IsRGBFormat(fbf);
  NTV2OutputXptID src = ::GetFrameStoreOutputXptFromChannel(ch, isRGB);
  if(isRGB)
  {
    conns.insert(NTV2Connection(::GetCSCInputXptFromChannel(ch), src));
    src = ::GetCSCOutputXptFromChannel(ch);
    // Reset the CSC to a known-good state: cards retain register state across
    // runs, so a prior app leaving the CSC in Enhanced (unprogrammed) mode
    // would make RGB->YUV emit black for everyone after.
    card.SetColorSpaceMethod(NTV2_CSC_Method_Original, ch);
    card.SetColorSpaceRGBBlackRange(NTV2_CSC_RGB_RANGE_FULL, ch);
  }
  conns.insert(NTV2Connection(::GetSDIOutputInputXpt(ch), src));
  card.SetSDIOutputStandard(ch, ::GetNTV2StandardFromVideoFormat(vf));
  card.SetSDIOutLevelAtoLevelBConversion(ch, false);
  card.SetSDIOutRGBLevelAConversion(ch, false);
  card.ApplySignalRoute(conns, /*replace=*/true);

  std::printf(
      "  device=%s CanDoFBF(ARGB)=%d CanDoFBF(YCbCr8)=%d\n",
      ::NTV2DeviceIDToString(card.GetDeviceID()).c_str(),
      card.features().CanDoFrameBufferFormat(NTV2_FBF_ARGB),
      card.features().CanDoFrameBufferFormat(NTV2_FBF_8BIT_YCBCR));

  // Draw a test pattern into a host buffer.
  NTV2FormatDescriptor fd(vf, fbf);
  NTV2Buffer host(fd.GetTotalBytes());
  if(fbf == NTV2_FBF_10BIT_YCBCR_422PL3_LE)
  {
    // NTV2TestPatternGen doesn't support planar formats, so fill the 3 planes
    // by hand: Y = horizontal 10-bit ramp (clearly a pattern, not uniform),
    // Cb/Cr = neutral 512. Layout per plane from the format descriptor; each
    // sample is a 16-bit LE word with the 10-bit value in the low 10 bits.
    host.Fill(uint16_t(0));
    auto* base = reinterpret_cast<uint8_t*>(host.GetHostPointer());
    const int H = int(fd.GetVisibleRasterHeight());
    std::size_t off = 0;
    for(int plane = 0; plane < 3; ++plane)
    {
      const int rowBytes = int(fd.GetBytesPerRow(plane));
      const int samples = rowBytes / 2; // 16-bit samples per row
      for(int y = 0; y < H; ++y)
      {
        auto* row = reinterpret_cast<uint16_t*>(base + off + std::size_t(y) * rowBytes);
        for(int x = 0; x < samples; ++x)
          row[x] = (plane == 0)
                       ? uint16_t((x * 1023 / (samples > 1 ? samples - 1 : 1)) & 0x3FF)
                       : uint16_t(512);
      }
      off += fd.GetTotalRasterBytes(plane);
    }
  }
  else if(NTV2TestPatternGen gen;
          !gen.DrawTestPattern(NTV2_TestPatt_ColorBars100, fd, host))
  {
    std::printf("ERROR: DrawTestPattern failed\n");
    return false;
  }
  {
    // Confirm the host framebuffer is actually non-black before DMA.
    const uint8_t* p = (const uint8_t*)host.GetHostPointer();
    long s = 0;
    const size_t nb = host.GetByteCount();
    for(size_t i = 0; i < nb; i += 257)
      s += p[i];
    std::printf("  host buffer avg byte=%.1f (non-zero => pattern present)\n",
                double(s) / double(nb / 257 + 1));
  }

  if(!autoCirc)
  {
    // Demo path: write one frame directly to the current output frame.
    uint32_t outFrame = 0;
    card.GetOutputFrame(ch, outFrame);
    return card.DMAWriteFrame(
        outFrame, reinterpret_cast<ULWord*>(host.GetHostPointer()),
        host.GetByteCount());
  }

  // Our path: AutoCirculate the same static frame repeatedly.
  card.AutoCirculateStop(ch);
  if(!card.AutoCirculateInitForOutput(ch, 7))
    return false;
  card.AutoCirculateStart(ch);
  for(int i = 0; i < 60; ++i)
  {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if(st.CanAcceptMoreOutputFrames())
    {
      AUTOCIRCULATE_TRANSFER xfer;
      xfer.SetVideoBuffer(
          reinterpret_cast<ULWord*>(host.GetHostPointer()), host.GetByteCount());
      card.AutoCirculateTransfer(ch, xfer);
    }
    std::this_thread::sleep_for(16ms);
  }
  return true;
}

// Minimal AutoCirculate capture of the SDI wire as 8-bit YCbCr (UYVY).
bool capture(CNTV2Card& card, NTV2VideoFormat vf, std::vector<uint8_t>& out, int& w, int& h)
{
  const NTV2Channel ch = NTV2_CHANNEL1;
  card.SetVideoFormat(vf, false, false, ch);
  card.SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR);
  card.EnableChannel(ch);
  card.SetMode(ch, NTV2_MODE_CAPTURE);
  if(card.features().HasBiDirectionalSDI())
    card.SetSDITransmitEnable(ch, false);
  card.Connect(
      ::GetFrameStoreInputXptFromChannel(ch),
      ::GetSDIInputOutputXptFromChannel(ch));

  NTV2FormatDescriptor fd(vf, NTV2_FBF_8BIT_YCBCR);
  w = fd.GetRasterWidth();
  h = fd.GetVisibleRasterHeight();
  NTV2Buffer host(fd.GetTotalBytes());

  card.AutoCirculateStop(ch);
  if(!card.AutoCirculateInitForInput(ch, 7))
    return false;
  card.AutoCirculateStart(ch);

  for(int i = 0; i < 120; ++i) // up to ~2s
  {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if(st.HasAvailableInputFrame())
    {
      AUTOCIRCULATE_TRANSFER xfer;
      xfer.SetVideoBuffer(
          reinterpret_cast<ULWord*>(host.GetHostPointer()), host.GetByteCount());
      if(card.AutoCirculateTransfer(ch, xfer))
      {
        out.assign(
            (uint8_t*)host.GetHostPointer(),
            (uint8_t*)host.GetHostPointer() + fd.GetTotalBytes());
        card.AutoCirculateStop(ch);
        return true;
      }
    }
    std::this_thread::sleep_for(16ms);
  }
  card.AutoCirculateStop(ch);
  return false;
}

// ============================================================================
// Bare-AJA 4K quad-link Squares round-trip (isolates output vs input vs card).
// Each quadrant is filled with a distinct luma so any scramble is unambiguous:
//   TL=64  TR=128  BL=192  BR=255  (neutral chroma 128).
// Mirrors ntv2player4k (non-12G squares) + ntv2capture4k routing exactly.
// ============================================================================
void fill4KQuadrants(NTV2Buffer& host, int w, int h)
{
  uint8_t* p = reinterpret_cast<uint8_t*>(host.GetHostPointer());
  const int rb = w * 2; // UYVY = 2 bytes/pixel
  const uint8_t L[4] = {64, 128, 192, 255}; // TL, TR, BL, BR
  for(int y = 0; y < h; ++y)
    for(int x = 0; x < w; ++x)
    {
      const int q = ((y < h / 2) ? 0 : 2) + ((x < w / 2) ? 0 : 1);
      uint8_t* px = p + std::ptrdiff_t(y) * rb + std::ptrdiff_t(x) * 2;
      px[0] = 128;    // chroma (U/V), neutral
      px[1] = L[q];   // luma
    }
}

bool setupOutput4KSquares(CNTV2Card& card, NTV2VideoFormat vf)
{
  const NTV2Channel ch = NTV2_CHANNEL1;
  const NTV2ChannelSet fs = ::NTV2MakeChannelSet(ch, 4);
  const NTV2FrameBufferFormat fbf = NTV2_FBF_8BIT_YCBCR;
  card.EnableChannels(fs, true);
  card.SetVideoFormat(fs, vf, false);
  card.SetReference(NTV2_REFERENCE_FREERUN);
  card.SetFrameBufferFormat(fs, fbf);
  card.SetTsiFrameEnable(false, ch);
  card.Set4kSquaresEnable(true, ch);
  for(NTV2Channel c : fs)
    card.SetMode(c, NTV2_MODE_DISPLAY);
  if(card.features().HasBiDirectionalSDI())
    card.SetSDITransmitEnable(fs, true);

  // FB[i]YUV -> SDIOut[i]  (== ntv2player4k RouteFsToSDIOut, non-12G squares)
  NTV2XptConnections conns;
  for(UWord i = 0; i < 4; ++i)
  {
    const NTV2Channel c = NTV2Channel(ch + i);
    conns.insert(NTV2Connection(
        ::GetSDIOutputInputXpt(c, false),
        ::GetFrameStoreOutputXptFromChannel(c, false /*RGB*/, false /*is425*/)));
  }
  card.ApplySignalRoute(conns, true);

  NTV2FormatDescriptor fd(vf, fbf);
  NTV2Buffer host(fd.GetTotalBytes());
  fill4KQuadrants(host, fd.GetRasterWidth(), fd.GetVisibleRasterHeight());

  card.AutoCirculateStop(ch);
  if(!card.AutoCirculateInitForOutput(ch, 7, NTV2_AUDIOSYSTEM_INVALID, 0, 1))
  {
    std::printf("ERROR: 4K AutoCirculateInitForOutput failed\n");
    return false;
  }
  card.AutoCirculateStart(ch);
  for(int i = 0; i < 60; ++i)
  {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if(st.CanAcceptMoreOutputFrames())
    {
      AUTOCIRCULATE_TRANSFER xfer;
      xfer.SetVideoBuffer(
          reinterpret_cast<ULWord*>(host.GetHostPointer()), host.GetByteCount());
      card.AutoCirculateTransfer(ch, xfer);
    }
    std::this_thread::sleep_for(16ms);
  }
  return true;
}

bool capture4KSquares(
    CNTV2Card& card, NTV2VideoFormat vf, std::vector<uint8_t>& out, int& w, int& h)
{
  const NTV2Channel ch = NTV2_CHANNEL1;
  const NTV2ChannelSet fs = ::NTV2MakeChannelSet(ch, 4);
  const NTV2FrameBufferFormat fbf = NTV2_FBF_8BIT_YCBCR;
  card.EnableChannels(fs, true);
  card.SetVideoFormat(fs, vf, false);
  card.SetFrameBufferFormat(fs, fbf);
  card.SetTsiFrameEnable(false, ch);
  card.Set4kSquaresEnable(true, ch);
  for(NTV2Channel c : fs)
    card.SetMode(c, NTV2_MODE_CAPTURE);
  if(card.features().HasBiDirectionalSDI())
    card.SetSDITransmitEnable(fs, false);

  // SDIIn[i] -> FB[i]  (1:1, == ntv2democommon squares+YUV)
  NTV2XptConnections conns;
  for(UWord i = 0; i < 4; ++i)
  {
    const NTV2Channel c = NTV2Channel(ch + i);
    conns.insert(NTV2Connection(
        ::GetFrameStoreInputXptFromChannel(c),
        ::GetSDIInputOutputXptFromChannel(c)));
  }
  card.ApplySignalRoute(conns, true);

  NTV2FormatDescriptor fd(vf, fbf);
  w = fd.GetRasterWidth();
  h = fd.GetVisibleRasterHeight();
  NTV2Buffer host(fd.GetTotalBytes());

  card.AutoCirculateStop(ch);
  if(!card.AutoCirculateInitForInput(ch, 7))
  {
    std::printf("ERROR: 4K AutoCirculateInitForInput failed\n");
    return false;
  }
  card.AutoCirculateStart(ch);
  for(int i = 0; i < 120; ++i)
  {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if(st.HasAvailableInputFrame())
    {
      AUTOCIRCULATE_TRANSFER xfer;
      xfer.SetVideoBuffer(
          reinterpret_cast<ULWord*>(host.GetHostPointer()), host.GetByteCount());
      if(card.AutoCirculateTransfer(ch, xfer))
      {
        out.assign(
            (uint8_t*)host.GetHostPointer(),
            (uint8_t*)host.GetHostPointer() + fd.GetTotalBytes());
        card.AutoCirculateStop(ch);
        return true;
      }
    }
    std::this_thread::sleep_for(16ms);
  }
  card.AutoCirculateStop(ch);
  return false;
}

int run4KSquares(CNTV2Card& cardOut, CNTV2Card& cardIn)
{
  const NTV2VideoFormat vf = NTV2_FORMAT_3840x2160p_5000; // UHDp50 (the failing case)
  std::printf("4K-squares repro: bare out->in  vf=UHDp50  quadrant luma "
              "TL=64 TR=128 BL=192 BR=255 (neutral chroma)\n");
  if(!setupOutput4KSquares(cardOut, vf))
  {
    std::printf("ERROR: 4K output setup failed\n");
    return 2;
  }
  std::this_thread::sleep_for(500ms);
  std::vector<uint8_t> frame;
  int w = 0, h = 0;
  if(!capture4KSquares(cardIn, vf, frame, w, h))
  {
    std::printf("RESULT: 4K capture got NO frame (no signal / no lock)\n");
    return 1;
  }
  const int rb = w * 2; // UYVY
  auto quadY = [&](int qx, int qy) -> double {
    long s = 0, n = 0;
    for(int y = qy; y < qy + h / 2; y += 8)
      for(int x = qx; x < qx + w / 2; x += 4)
      {
        s += frame[std::size_t(y) * rb + std::size_t(x) * 2 + 1]; // Y byte
        ++n;
      }
    return n ? double(s) / n : 0;
  };
  const double tl = quadY(0, 0), tr = quadY(w / 2, 0);
  const double bl = quadY(0, h / 2), br = quadY(w / 2, h / 2);
  std::printf(
      "RESULT: captured %dx%d  quadrant Y avg: TL=%.0f TR=%.0f BL=%.0f BR=%.0f\n",
      w, h, tl, tr, bl, br);
  std::printf("        EXPECTED: TL=64 TR=128 BL=192 BR=255  => %s\n",
              (std::abs(tl - 64) < 20 && std::abs(tr - 128) < 20
               && std::abs(bl - 192) < 20 && std::abs(br - 255) < 20)
                  ? "BARE PATH OK (bug is in score)"
                  : "BARE PATH ALSO WRONG (card/cabling/firmware quad-link)");
  return 0;
}

// Read-only firmware enumeration: report the current personality and the set
// of dynamically-loadable device IDs (the "firmwares" the AJA GUI shows).
// Does NOT switch anything.
void enumFirmware(CNTV2Card& card, int idx)
{
  const char* kFwDir = "C:/ProgramData/AJA/ntv2/Firmware";
  card.AddDynamicDirectory(kFwDir);

  const NTV2DeviceID cur = card.GetDeviceID();
  std::printf("card%d: %s\n", idx, card.GetDisplayName().c_str());
  std::printf("  current deviceID : 0x%08x (%s)\n", unsigned(cur),
              ::NTV2DeviceIDToString(cur).c_str());
  std::printf("  IsDynamicDevice  : %s\n",
              card.IsDynamicDevice() ? "yes" : "no");
  const NTV2DeviceID base = card.GetBaseDeviceID();
  std::printf("  baseDeviceID     : 0x%08x (%s)\n", unsigned(base),
              ::NTV2DeviceIDToString(base).c_str());

  const NTV2DeviceIDList list = card.GetDynamicDeviceList();
  std::printf("  loadable firmwares (%zu):\n", list.size());
  for(const NTV2DeviceID id : list)
    std::printf("    - 0x%08x  %-22s  loadable=%s\n", unsigned(id),
                ::NTV2DeviceIDToString(id).c_str(),
                card.CanLoadDynamicDevice(id) ? "yes" : "no");

  // Capability flags (verify our routing/format gating against the manual).
  auto& f = card.features();
  std::printf("  caps: 12gRouting=%d 4K=%d 8K=%d CSC(LUT)=%d multiFmt=%d rgbAlphaOut=%d\n",
              f.CanDo12gRouting(), f.CanDo4KVideo(), f.CanDo8KVideo(),
              f.CanDoColorCorrection(), f.CanDoMultiFormat(), f.CanDoRGBPlusAlphaOut());
  std::printf("  videofmt: UHD_SL(3840p50)=%d UHD_quad(4x1920p50)=%d 8K(UHD2p50)=%d\n",
              f.CanDoVideoFormat(NTV2_FORMAT_3840x2160p_5000),
              f.CanDoVideoFormat(NTV2_FORMAT_4x1920x1080p_5000),
              f.CanDoVideoFormat(NTV2_FORMAT_4x3840x2160p_5000));
  std::printf("  fbf: YUV8=%d YUV10=%d RGB8(ARGB)=%d RGB10=%d RGB12(48b)=%d RGB12P=%d 422PL3=%d\n",
              f.CanDoFrameBufferFormat(NTV2_FBF_8BIT_YCBCR),
              f.CanDoFrameBufferFormat(NTV2_FBF_10BIT_YCBCR),
              f.CanDoFrameBufferFormat(NTV2_FBF_ARGB),
              f.CanDoFrameBufferFormat(NTV2_FBF_10BIT_RGB),
              f.CanDoFrameBufferFormat(NTV2_FBF_48BIT_RGB),
              f.CanDoFrameBufferFormat(NTV2_FBF_12BIT_RGB_PACKED),
              f.CanDoFrameBufferFormat(NTV2_FBF_10BIT_YCBCR_422PL3_LE));
}

NTV2DeviceID firmwareNameToID(const std::string& n)
{
  if(n == "retail" || n == "4k" || n == "kona5")
    return DEVICE_ID_KONA5;
  if(n == "8k")
    return DEVICE_ID_KONA5_8K;
  if(n == "12bit" || n == "2x4k")
    return DEVICE_ID_KONA5_2X4K;
  return DEVICE_ID_INVALID;
}

// Dynamic (non-persistent) firmware swap via partial FPGA reconfig. Reverts on
// power-cycle; does NOT touch the PROM. Returns true if the card ends on target.
bool loadFirmware(CNTV2Card& card, int idx, NTV2DeviceID target)
{
  card.AddDynamicDirectory("C:/ProgramData/AJA/ntv2/Firmware");
  const NTV2DeviceID cur = card.GetDeviceID();
  if(cur == target)
  {
    std::printf("card%d: already on %s, no switch\n", idx,
                ::NTV2DeviceIDToString(target).c_str());
    return true;
  }
  if(!card.CanLoadDynamicDevice(target))
  {
    std::printf("card%d: ERROR cannot load %s (not in dynamic list)\n", idx,
                ::NTV2DeviceIDToString(target).c_str());
    return false;
  }
  std::printf("card%d: loading %s (from %s)...\n", idx,
              ::NTV2DeviceIDToString(target).c_str(),
              ::NTV2DeviceIDToString(cur).c_str());
  if(!card.LoadDynamicDevice(target))
  {
    std::printf("card%d: ERROR LoadDynamicDevice failed\n", idx);
    return false;
  }
  const NTV2DeviceID now = card.GetDeviceID();
  std::printf("card%d: now %s (0x%08x) %s\n", idx,
              ::NTV2DeviceIDToString(now).c_str(), unsigned(now),
              now == target ? "OK" : "MISMATCH");
  return now == target;
}
} // namespace

int main(int argc, char** argv)
{
  int outDev = 0, inDev = 1;
  bool yuv = false, autoCirc = false, pl3 = false, fourK = false, fwEnum = false;
  std::string save, loadFw;
  for(int i = 1; i < argc; ++i)
  {
    std::string a = argv[i];
    if(a == "--yuv")
      yuv = true;
    else if(a == "--pl3")
      pl3 = true;
    else if(a == "--4k")
      fourK = true;
    else if(a == "--fw")
      fwEnum = true;
    else if(a == "--load" && i + 1 < argc)
      loadFw = argv[++i];
    else if(a == "--ac")
      autoCirc = true;
    else if(a == "--out" && i + 1 < argc)
      outDev = std::atoi(argv[++i]);
    else if(a == "--in" && i + 1 < argc)
      inDev = std::atoi(argv[++i]);
    else if(a == "--save" && i + 1 < argc)
      save = argv[++i];
  }

  const NTV2VideoFormat vf = NTV2_FORMAT_1080p_5994_A;
  const NTV2FrameBufferFormat fbf
      = pl3 ? NTV2_FBF_10BIT_YCBCR_422PL3_LE
            : (yuv ? NTV2_FBF_8BIT_YCBCR : NTV2_FBF_ARGB);
  const char* fbfName = pl3 ? "10BIT_YCBCR_422PL3_LE"
                            : (yuv ? "8BIT_YCBCR" : "ARGB");
  std::printf(
      "repro: out=card%d in=card%d fbf=%s path=%s\n", outDev, inDev, fbfName,
      autoCirc ? "AutoCirculate" : "DMAWriteFrame");

  CNTV2Card cardOut, cardIn;
  if(!openCard(outDev, cardOut))
  {
    std::printf("ERROR: cannot open output card %d\n", outDev);
    return 2;
  }
  if(!openCard(inDev, cardIn))
  {
    std::printf("ERROR: cannot open input card %d\n", inDev);
    return 2;
  }

  if(fwEnum)
  {
    enumFirmware(cardOut, outDev);
    enumFirmware(cardIn, inDev);
    return 0;
  }

  if(!loadFw.empty())
  {
    const NTV2DeviceID target = firmwareNameToID(loadFw);
    if(target == DEVICE_ID_INVALID)
    {
      std::printf("ERROR: unknown firmware '%s' (use retail|8k|12bit)\n",
                  loadFw.c_str());
      return 2;
    }
    const bool a = loadFirmware(cardOut, outDev, target);
    const bool b = loadFirmware(cardIn, inDev, target);
    return (a && b) ? 0 : 1;
  }

  if(fourK)
    return run4KSquares(cardOut, cardIn);

  if(!setupOutput(cardOut, vf, fbf, autoCirc))
  {
    std::printf("ERROR: output setup failed\n");
    return 2;
  }
  std::this_thread::sleep_for(500ms); // let the link settle

  std::vector<uint8_t> frame;
  int w = 0, h = 0;
  if(!capture(cardIn, vf, frame, w, h))
  {
    std::printf("RESULT: capture got NO frame (no signal / no lock)\n");
    return 1;
  }

  // UYVY: bytes are U Y0 V Y1; sample luma (Y) across the frame.
  int ymin = 255, ymax = 0;
  long ysum = 0, n = 0;
  const int rowBytes = w * 2;
  for(int y = 0; y < h; y += 8)
    for(int x = 1; x < rowBytes; x += 4) // Y0 positions
    {
      int Y = frame[size_t(y) * rowBytes + x];
      ymin = std::min(ymin, Y);
      ymax = std::max(ymax, Y);
      ysum += Y;
      ++n;
    }
  const double yavg = n ? double(ysum) / n : 0;

  if(!save.empty())
  {
    // Save UYVY luma as a grayscale PGM for inspection.
    FILE* f = std::fopen(save.c_str(), "wb");
    if(f)
    {
      std::fprintf(f, "P5\n%d %d\n255\n", w, h);
      for(int y = 0; y < h; ++y)
        for(int x = 0; x < w; ++x)
          std::fputc(frame[size_t(y) * rowBytes + x * 2 + 1], f);
      std::fclose(f);
    }
  }

  const bool black = yavg < 32; // black level (Y~16); color bars avg ~125
  std::printf(
      "RESULT: captured %dx%d  Y min=%d max=%d avg=%.1f  => %s\n", w, h, ymin,
      ymax, yavg, black ? "BLACK" : "HAS PATTERN");
  return black ? 1 : 0;
}
