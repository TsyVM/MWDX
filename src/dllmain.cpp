// MWDX entry point.
//
// This DLL is dropped next to speed.exe under the name d3d9.dll. Windows
// resolves an executable's imports from its own directory before the system
// directory, so the game links against us instead of the real runtime without
// any patching of the executable. Everything the game asks for arrives through
// the eight exports below; MWDX.def pins their ordinals and names so the
// import table the game was built against still resolves.
//
// The only decision made here is which backend the game gets. Direct3DCreate9
// hands back one of three objects that all implement IDirect3D9: our D3D11
// root, our D3D12 root, or a thin wrapper around the system runtime. From the
// game's point of view they are indistinguishable.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <new>
#include <string>

#include "Logger.h"
#include "directx/WrappedIDirect3D9.h"
#include <core/BackendSelect.h>
#include <d3d9proxy/D9Root.h>
#include <d3d9proxy12/D9Root12.h>

// The system d3d9.dll, loaded by full path in DllMain. Only the passthrough
// backend uses these; the translating backends never touch the real runtime.
static HMODULE s_realD3D9 = nullptr;
using PFN_Direct3DCreate9   = IDirect3D9* (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
static PFN_Direct3DCreate9   s_realCreate9   = nullptr;
static PFN_Direct3DCreate9Ex s_realCreate9Ex = nullptr;

extern "C" {

// PIX instrumentation hooks. The game calls these unconditionally, so they
// have to exist and be cheap; they carry no meaning outside a PIX capture.
// D3DPERF_GetStatus returning 0 tells the caller no profiler is attached,
// which is what stops the game emitting event markers at all.
int   WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR)     { return 0; }
int   WINAPI D3DPERF_EndEvent()                        { return 0; }
DWORD WINAPI D3DPERF_GetStatus()                       { return 0; }
BOOL  WINAPI D3DPERF_QueryRepeatFrame()                { return FALSE; }
void  WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR)     {}
void  WINAPI D3DPERF_SetOptions(DWORD)                 {}
void  WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR)     {}
void  WINAPI DebugSetMute()                            {}

// Emitted once, on whichever create call comes first. Configured() is what the
// ini asked for; Active() is what survived probing the adapter. When they
// differ the user needs to know their setting was overruled and why, because
// the symptom otherwise is "my setting does nothing".
static void LogBackendChoice()
{
    using namespace dx9to11;
    static bool s_logged = false;
    if (s_logged) return;
    s_logged = true;

    const auto want = BackendSelect::Configured();
    const auto got  = BackendSelect::Active();
    if (want == got)
        Logger::Log("Renderer backend: %s", BackendName(got));
    else
        Logger::Log("Renderer backend: %s requested, %s in use "
                    "(requested API unavailable on this adapter)",
                    BackendName(want), BackendName(got));
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVer)
{
    using namespace dx9to11;
    LogBackendChoice();
    switch (BackendSelect::Active()) {
    case Backend::DX11:
        return new D9Root(sdkVer);
    case Backend::DX12:
        return new D9Root12(sdkVer);
    case Backend::DX9:
    default:
        break;
    }

    // Passthrough. BackendSelect only resolves to DX9 when it has already
    // confirmed the real entry point exists, so this branch failing means the
    // system runtime disappeared between probe and call.
    if (!s_realCreate9) {
        Logger::Log("ERROR: Direct3DCreate9 — DX9 passthrough selected but the "
                    "system d3d9.dll has no Direct3DCreate9");
        return nullptr;
    }
    IDirect3D9* pReal = s_realCreate9(sdkVer);
    if (!pReal) {
        Logger::Log("ERROR: system Direct3DCreate9 returned null");
        return nullptr;
    }
    return new WrappedIDirect3D9(pReal);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVer, IDirect3D9Ex** ppD3D)
{
    using namespace dx9to11;
    if (!ppD3D) return D3DERR_INVALIDCALL;
    *ppD3D = nullptr;
    LogBackendChoice();

    switch (BackendSelect::Active()) {
    case Backend::DX11: {
        // Construct at the IDirect3D9 refcount of 1, then let QueryInterface
        // take the reference the caller keeps. The Release below drops our
        // construction reference, so a QI failure destroys the object rather
        // than leaking it.
        auto* root = new (std::nothrow) D9Root(sdkVer);
        if (!root) return E_OUTOFMEMORY;
        const HRESULT hr = root->QueryInterface(__uuidof(IDirect3D9Ex),
                                                reinterpret_cast<void**>(ppD3D));
        root->Release();
        return hr;
    }
    case Backend::DX12: {
        auto* root = new (std::nothrow) D9Root12(sdkVer);
        if (!root) return E_OUTOFMEMORY;
        const HRESULT hr = root->QueryInterface(__uuidof(IDirect3D9Ex),
                                                reinterpret_cast<void**>(ppD3D));
        root->Release();
        return hr;
    }
    case Backend::DX9:
    default:
        break;
    }
    if (!s_realCreate9Ex) return E_NOTIMPL;
    return s_realCreate9Ex(sdkVer, ppD3D);
}

// D3D9On12 is Microsoft's own D3D9-over-D3D12 mapping layer; these entry
// points let a caller hand it an existing D3D12 device to render through.
//
// MWDX exports them so that anything looking for them still links, but it
// ignores the supplied device and routes into its own factories. Adopting a
// caller's device would mean running the game's D3D9 calls through someone
// else's translation layer, which is exactly the thing this project sets out
// to replace with its own.
IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdkVer, void*, UINT)
{
    return Direct3DCreate9(sdkVer);
}

HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdkVer, void*, UINT, IDirect3D9Ex** ppD3D)
{
    return Direct3DCreate9Ex(sdkVer, ppD3D);
}

}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Nothing here is per-thread, and the game spawns threads freely.
        DisableThreadLibraryCalls(hInst);

        // Logs go beside the executable, not beside this DLL: the two are the
        // same directory for a normal install, but the executable is the one
        // the user will think to look in.
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string gameDir = exePath;
        auto slash = gameDir.rfind('\\');
        if (slash != std::string::npos) gameDir.resize(slash + 1);

        Logger::Init((gameDir + "MWDX.log").c_str());
        Logger::Log("MWDX loading — game dir: %s", gameDir.c_str());

        // Absolute path into system32 is mandatory. A bare LoadLibrary("d3d9")
        // searches the executable's directory first and would find this DLL,
        // which loads us into ourselves.
        char sys32[MAX_PATH];
        GetSystemDirectoryA(sys32, MAX_PATH);
        s_realD3D9 = LoadLibraryA((std::string(sys32) + "\\d3d9.dll").c_str());
        if (s_realD3D9) {
            s_realCreate9   = reinterpret_cast<PFN_Direct3DCreate9>  (GetProcAddress(s_realD3D9, "Direct3DCreate9"));
            s_realCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(s_realD3D9, "Direct3DCreate9Ex"));
        }

        // Not fatal: the translating backends need none of this. Only
        // Backend=1 is lost, and BackendSelect will promote away from it.
        if (!s_realCreate9)
            Logger::Log("WARN: system Direct3DCreate9 not found — DX9 passthrough disabled");
        else
            Logger::Log("Real d3d9.dll loaded OK");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Logger::Shutdown();
        if (s_realD3D9) { FreeLibrary(s_realD3D9); s_realD3D9 = nullptr; }
    }
    return TRUE;
}
