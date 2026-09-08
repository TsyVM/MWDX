// IDirect3DVolumeTexture9 -- a 3-D texture.
//
// Backed by an ID3D11Texture3D. Used far less than the 2-D path but not
// optional: shaders sample volume textures for effects like the metallic flake
// in car paint, and a game that asks for one and cannot have it loses that
// effect entirely.
//
// The row and slice pitches reported from a lock must be the ones D3D11
// actually gives back, not values computed from the format. Drivers pad rows
// and slices for alignment, and a caller writing at a computed pitch corrupts
// every row after the first.

#pragma once

#ifndef DX9TO11_D9VOLUMETEXTURE_H
#define DX9TO11_D9VOLUMETEXTURE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9VolumeTexture final : public IDirect3DVolumeTexture9 {
public:
    D9VolumeTexture(
        D9Device*                        pDevice,
        UINT                              width,
        UINT                              height,
        UINT                              depth,
        UINT                              levels,
        DWORD                             usage,
        D3DFORMAT                         format,
        D3DPOOL                           pool,
        ComPtr<ID3D11Texture3D>           texture,
        ComPtr<ID3D11ShaderResourceView>  srv,
        ComPtr<ID3D11Texture3D>           staging) noexcept;

    D9VolumeTexture(const D9VolumeTexture&)            = delete;
    D9VolumeTexture& operator=(const D9VolumeTexture&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override;
    DWORD   STDMETHODCALLTYPE GetLOD() override;
    DWORD   STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override;

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9** ppVolumeLevel) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(CONST D3DBOX* pDirtyBox) override;

    [[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return m_srv.Get(); }

    [[nodiscard]] ID3D11ShaderResourceView* SRV(bool srgb) const noexcept;

private:
    ~D9VolumeTexture();

    D9Device*                        m_device;
    UINT                              m_width, m_height, m_depth, m_levels;
    DWORD                             m_usage;
    D3DFORMAT                         m_format;
    D3DPOOL                           m_pool;
    ComPtr<ID3D11Texture3D>           m_texture;
    ComPtr<ID3D11ShaderResourceView>  m_srv;
    ComPtr<ID3D11Texture3D>           m_staging;

    std::atomic<ULONG>    m_refCount{ 1 };
    DWORD                 m_lod{};
    D3DTEXTUREFILTERTYPE  m_autoGenFilter{ D3DTEXF_LINEAR };

    int    m_lockedLevel{ -1 };
    DWORD  m_lockFlags{};

    mutable bool m_srgbGapWarned{ false };
};

}

#endif
