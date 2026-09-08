// IDirect3DTexture9 -- a 2-D texture with its mip chain.
//
// Backed by an ID3D11Texture2D plus the views the pipeline needs. Two shader
// resource views are kept rather than one because D3D9 selects sRGB decoding
// with a sampler state (D3DSAMP_SRGBTEXTURE) that the game may flip at any
// time, whereas in D3D11 the decision is baked into the view's format. Keeping
// a linear view and an sRGB view and choosing between them at bind time is far
// cheaper than recreating a view mid-frame.
//
// The optional staging texture exists to serve LockRect. D3D9 lets a game map
// almost any texture; D3D11 only allows it on a resource created for staging.
// A texture the game may lock therefore gets a CPU-accessible companion that
// is copied to and from the GPU resource around the lock.

#pragma once

#ifndef DX9TO11_D9TEXTURE_H
#define DX9TO11_D9TEXTURE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <d3d9proxy/D9Surface.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9Texture final : public IDirect3DTexture9 {
public:
    D9Texture(
        D9Device*                        pDevice,
        UINT                              width,
        UINT                              height,
        UINT                              levels,
        DWORD                             usage,
        D3DFORMAT                         format,
        D3DPOOL                           pool,
        ComPtr<ID3D11Texture2D>           texture,
        ComPtr<ID3D11ShaderResourceView>  srv,
        ComPtr<ID3D11Texture2D>           staging,
        ComPtr<ID3D11ShaderResourceView>  srvSrgb = nullptr) noexcept;

    D9Texture(const D9Texture&)            = delete;
    D9Texture& operator=(const D9Texture&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override;
    DWORD   STDMETHODCALLTYPE GetLOD() override;
    DWORD   STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override;

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT* pDirtyRect) override;

    [[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return m_srv.Get(); }

    [[nodiscard]] ID3D11ShaderResourceView* SRV(bool srgb) const noexcept
    {
        return (srgb && m_srvSrgb) ? m_srvSrgb.Get() : m_srv.Get();
    }

    [[nodiscard]] bool HasSRGBVariant() const noexcept { return m_srvSrgb != nullptr; }

private:
    ~D9Texture();

    [[nodiscard]] HRESULT GetOrCreateLevelSurface(UINT level, D9Surface** ppOut) noexcept;

    D9Device*                         m_device;
    UINT                               m_width, m_height, m_levels;
    DWORD                              m_usage;
    D3DFORMAT                          m_format;
    D3DPOOL                            m_pool;
    ComPtr<ID3D11Texture2D>            m_texture;
    ComPtr<ID3D11ShaderResourceView>   m_srv;
    ComPtr<ID3D11ShaderResourceView>   m_srvSrgb;
    ComPtr<ID3D11Texture2D>            m_staging;

    std::vector<D9Surface*> m_levelSurfaces;

    std::atomic<ULONG> m_refCount{ 1 };
    DWORD              m_lod{};
    D3DTEXTUREFILTERTYPE m_autoGenFilter{ D3DTEXF_LINEAR };
};

}

#endif
