// Which renderer is in use, and every setting read from MWDX.ini.
//
// Configured() is what the user asked for; Active() is what the machine can
// actually provide, after probing for the requested API and falling back if it
// is unavailable. They differ often enough that anything reporting to the user
// should report both.
//
// Everything here is resolved once, lazily, on first use. Probing for a
// graphics adapter initialises drivers, which cannot be done safely while the
// DLL is still loading, so none of it can happen at load time.

#pragma once

#include <d3d9.h>

namespace dx9to11 {

enum class Backend : int {
    DX9  = 1,
    DX11 = 2,
    DX12 = 3,
};

constexpr const char* BackendName(Backend b) noexcept
{
    switch (b) {
    case Backend::DX9:  return "DirectX 9 (passthrough)";
    case Backend::DX11: return "DirectX 11";
    case Backend::DX12: return "DirectX 12";
    }
    return "unknown";
}

namespace BackendSelect {

bool PerfLogEnabled();

bool HalfPixelFixEnabled();

bool LegacyColorDefault();

bool ShaderDiskCacheEnabled();

bool ShaderModsEnabled();

bool ShaderDumpEnabled();

bool LowLatencyEnabled();

bool NoBindCache();
bool StateBlockRecordOnly();
unsigned UploadRingMB();
bool DebugLayerEnabled();

bool GpuValidationEnabled();

bool VerboseLogEnabled();

bool BorderlessFullscreenEnabled();

bool DumpFrameEnabled();

unsigned CensusLimit();

FARPROC RealD3D9Export(const char* name);

Backend Active();

Backend Configured();

IDirect3D9* CreateReal9(UINT sdkVersion);
HRESULT     CreateReal9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);

}
}
