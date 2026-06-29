#pragma once
// Single include point for the DeckLink COM SDK. The C++ header is generated
// from the SDK .idl by widl at build time (see CMakeLists.txt); it needs the
// Windows COM base types, so windows.h comes first.
#include <windows.h>

#include "DeckLinkAPI_h.h"
