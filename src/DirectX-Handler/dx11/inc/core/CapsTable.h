// Builds the D3DCAPS9 structure returned by GetDeviceCaps.
//
// Shared by both translating backends so there is exactly one capability table
// and the two cannot drift apart. See the implementation for why the contents
// of this structure deserve more care than they appear to: an under-reported
// capability makes a game silently abandon a feature, with no error raised
// anywhere.

#pragma once

#ifndef DX9TO11_CAPS_TABLE_H
#define DX9TO11_CAPS_TABLE_H

#include <d3d9.h>

namespace dx9to11 {

void SynthesiseCaps(UINT adapterOrdinal, D3DCAPS9* pCaps) noexcept;

}

#endif
