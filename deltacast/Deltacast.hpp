#pragma once
/**
 * @file Deltacast.hpp
 * @brief Umbrella include for the DELTACAST VideoMaster SDK (pure C API + lib;
 *        no codegen). Windows-focused, like the DeckLink backend.
 */

#if !defined(__linux__) && !defined(__APPLE__)
#include <windows.h>
#endif

#include <VideoMasterHD_Core.h>
#include <VideoMasterHD_Sdi.h>
