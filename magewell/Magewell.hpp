#pragma once
/**
 * @file Magewell.hpp
 * @brief Umbrella include for the Magewell Pro Capture SDK (LibMWCapture; pure C
 *        API + import lib, no codegen). Windows-only, capture (input) only —
 *        Magewell PCIe cards have no playout, so there is no output backend.
 *
 * MWCapture.h already pulls in <Windows.h>, <ks.h>/<ksmedia.h> and MWFOURCC.h;
 * we include <windows.h> first anyway (CreateEvent / WaitForSingleObject live
 * there) and MWFOURCC.h explicitly for the FOURCC constants + stride/size
 * helpers (FOURCC_CalcMinStride / FOURCC_CalcImageSize).
 */

#include <windows.h>

#include <LibMWCapture/MWCapture.h>

#include <MWFOURCC.h>
