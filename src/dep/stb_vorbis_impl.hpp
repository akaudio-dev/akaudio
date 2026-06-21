#pragma once
// Declarations-only view of the vendored stb_vorbis (implementation lives in
// stb_vorbis_impl.cpp). Include this anywhere that needs the stb_vorbis API.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
