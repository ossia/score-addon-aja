#!/usr/bin/env bash
# Generate the DeckLink COM C++ header + IID source from the SDK .idl files
# using MinGW's widl (the midl equivalent available in clang64).
#
#   $1 = SDK .idl dir (blackmagic-sdk/Win/include)
#   $2 = output/gen dir
#   $3 = widl executable
#
# widl doesn't treat the C++ `bool` keyword as a builtin the way MS MIDL does,
# so we map it to widl's 1-byte `boolean` in build-local copies of the .idl
# (the vendor files are never modified). For an MSVC build this whole step is
# replaced by midl.exe on the originals.
set -euo pipefail
IDL_DIR="$1"; GEN="$2"; WIDL="$3"
mkdir -p "$GEN"
cp "$IDL_DIR"/*.idl "$GEN"/
sed -i -E 's/\bbool\b/boolean/g' "$GEN"/*.idl
"$WIDL" -h -I "$GEN" -o "$GEN/DeckLinkAPI_h.h" "$GEN/DeckLinkAPI.idl"
"$WIDL" -u -I "$GEN" -o "$GEN/DeckLinkAPI_i.c" "$GEN/DeckLinkAPI.idl"
