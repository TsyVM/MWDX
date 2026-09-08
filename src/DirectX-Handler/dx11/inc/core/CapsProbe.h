// Runtime queries about what the adapter supports.
//
// Answers CheckDeviceFormat, CheckDeviceMultiSampleType and their relatives by
// asking D3D11 rather than guessing, so the answer reflects the machine the
// game is actually running on.
//
// Be permissive here. These are query APIs, and a game told "no" does not
// fail -- it stops asking and quietly does without the feature, leaving no
// error to trace. Answering "no" to something that would in fact have worked
// is therefore an expensive mistake, and a much harder one to notice than
// answering "yes" and having the creation fail visibly.

#pragma once

#ifndef DX9TO11_CAPS_PROBE_H
#define DX9TO11_CAPS_PROBE_H

#include <d3d9.h>
#include <d3d11.h>
#include <cstdint>

namespace dx9to11 {
namespace CapsProbe {

[[nodiscard]] bool FormatSupportsUsage(ID3D11Device* dev,
                                       D3DFORMAT     fmt,
                                       DWORD         usage,
                                       D3DRESOURCETYPE rtype) noexcept;

[[nodiscard]] UINT MultisampleQualityLevels(ID3D11Device* dev,
                                            D3DFORMAT     fmt,
                                            UINT          sampleCount) noexcept;

[[nodiscard]] bool DepthStencilSupported(ID3D11Device* dev,
                                         D3DFORMAT     dsFmt) noexcept;

}
}

#endif
