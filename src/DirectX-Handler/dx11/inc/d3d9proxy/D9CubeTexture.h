// IDirect3DCubeTexture9 -- a six-faced environment map.
//
// Backed by a single ID3D11Texture2D with an array size of six, which is how
// D3D11 represents a cube map. Face order matches D3D9's D3DCUBEMAP_FACES
// enumeration (+X, -X, +Y, -Y, +Z, -Z), and that ordering is also the array
// slice order in D3D11, so no remapping is needed.
//
// Subresource indices follow D3D11's convention: slice * mipLevels + level.
// Getting that arithmetic wrong is easy and shows up as a cube map whose faces
// are shuffled, which reads as a scene lit from impossible directions rather
// than as an indexing bug.

#pragma once

#ifndef DX9TO11_D9CUBETEXTURE_H
#define DX9TO11_D9CUBETEXTURE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <vector>
#include <d3d9proxy/D9Surface.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9CubeTexture final : public IDirect3DCubeTexture9 {
public:
    struct Face {
        ComPtr<ID3D11Texture2D>          texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Texture2D>          staging;
    };

    D9CubeTexture(
        D9Device*                 pDevice,
        UINT                       edgeLength,
        UINT                       levels,
        DWORD                      usage,
        D3DFORMAT                  format,
        D3DPOOL                    pool,
        std::array<Face, 6>        faces) noexcept;

    D9CubeTexture(const D9CubeTexture&)            = delete;
    D9CubeTexture& operator=(const D9CubeTexture&) = delete;

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

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppCubeMapSurface) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, CONST RECT* pDirtyRect) override;

    [[nodiscard]] ID3D11ShaderResourceView* SRV() noexcept;

    [[nodiscard]] ID3D11ShaderResourceView* SRV(bool srgb) noexcept;

    void MarkCombinedDirty() noexcept { m_cubeSrvDirty = true; }

private:
    ~D9CubeTexture();

    [[nodiscard]] HRESULT GetOrCreateFaceLevelSurface(UINT face, UINT level, D9Surface** ppOut) noexcept;
    HRESULT RebuildCombinedSRV() noexcept;

    D9Device*  m_device;
    UINT       m_edgeLength, m_levels;
    DWORD      m_usage;
    D3DFORMAT  m_format;
    D3DPOOL    m_pool;
    std::array<Face, 6> m_faces;

    std::array<std::vector<D9Surface*>, 6> m_faceSurfaces;

    ComPtr<ID3D11Texture2D>          m_cubeTex;
    ComPtr<ID3D11ShaderResourceView> m_cubeSrv;
    ComPtr<ID3D11ShaderResourceView> m_cubeSrvSrgb;
    bool                             m_cubeSrvDirty{ true };

    std::atomic<ULONG> m_refCount{ 1 };
    DWORD               m_lod{};
    D3DTEXTUREFILTERTYPE m_autoGenFilter{ D3DTEXF_LINEAR };
};

}

#endif
