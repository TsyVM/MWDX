// IDirect3DSurface9 -- one 2-D image: a render target, a depth-stencil buffer,
// a back buffer, or a single mip level of a texture.
//
// D3D9 treats a surface as an object in its own right; D3D11 has no such
// concept, only resources and views onto them. A surface here is therefore a
// reference to a subresource of some ID3D11Texture2D, plus whichever views
// that use requires -- a render target view, a depth-stencil view, or both.
//
// The `subresource` index is what makes a surface that belongs to a texture
// work. GetSurfaceLevel hands back a surface naming one mip level of the
// parent texture rather than an independent image, and the parent's lifetime
// has to outlast it.
//
// Back-buffer surfaces need particular care. A swap chain has several physical
// buffers that rotate, but D3D9 presents the game with one logical back buffer
// and quietly maps it to whichever is current. Games cache the pointer they
// get from GetBackBuffer and reuse it for the life of the device, so a surface
// that captured a specific physical buffer at creation time would name a stale
// one within a frame or two. The surface must resolve the current buffer at
// the point of use, not at the point of creation.

#pragma once

#ifndef DX9TO11_D9SURFACE_H
#define DX9TO11_D9SURFACE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <core/DeviceContext11.h>
#include <core/ResourceManager.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;
class D9CubeTexture;

class D9Surface final : public IDirect3DSurface9 {
public:

    struct Desc {
        UINT                 width        = 0;
        UINT                 height       = 0;
        D3DFORMAT            format       = D3DFMT_UNKNOWN;
        D3DPOOL              pool         = D3DPOOL_DEFAULT;
        DWORD                usage        = 0;
        D3DMULTISAMPLE_TYPE  multiSample  = D3DMULTISAMPLE_NONE;
        UINT                 subresource  = 0;
        BOOL                 lockable     = FALSE;
    };

    D9Surface(
        D9Device*                      pDevice,
        const Desc&                    desc,
        ComPtr<ID3D11Texture2D>        texture,
        ComPtr<ID3D11Texture2D>        staging,
        ComPtr<ID3D11RenderTargetView> rtv,
        ComPtr<ID3D11DepthStencilView> dsv,
        ComPtr<ID3D11ShaderResourceView> srv,
        IUnknown*                      container,

        ComPtr<ID3D11RenderTargetView> rtvSrgb = nullptr) noexcept;

    D9Surface(const D9Surface&)            = delete;
    D9Surface& operator=(const D9Surface&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void* pData, DWORD SizeOfData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void* pData, DWORD* pSizeOfData) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override;

    [[nodiscard]] ID3D11Texture2D* Texture()     const noexcept { return m_texture.Get(); }
    [[nodiscard]] ID3D11Texture2D* Staging()     const noexcept { return m_staging.Get(); }
    [[nodiscard]] UINT             Subresource() const noexcept { return m_desc.subresource; }
    [[nodiscard]] const Desc&      Info()        const noexcept { return m_desc; }
    [[nodiscard]] ID3D11RenderTargetView*   RTV() const noexcept { return m_rtv.Get(); }
    [[nodiscard]] ID3D11DepthStencilView*   DSV() const noexcept { return m_dsv.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* SRV() const noexcept { return m_srv.Get(); }

    [[nodiscard]] ID3D11RenderTargetView* RTV(bool srgb) const noexcept
    {
        return (srgb && m_rtvSrgb) ? m_rtvSrgb.Get() : m_rtv.Get();
    }

    [[nodiscard]] bool HasSRGBVariant() const noexcept { return m_rtvSrgb != nullptr; }

    [[nodiscard]] ID3D11ShaderResourceView* GetOrCreateBlitSRV() noexcept;

    void SetCubeOwner(D9CubeTexture* owner) noexcept { m_cubeOwner = owner; }

    void SetAutogenOwner(class D9Texture* owner) noexcept { m_autogenOwner = owner; }

    void NotifyContentsChanged() noexcept;

private:

    friend class D9Texture;
    friend class D9CubeTexture;

    ~D9Surface();

    D9Device*                        m_device;
    Desc                              m_desc;
    ComPtr<ID3D11Texture2D>           m_texture;
    ComPtr<ID3D11Texture2D>           m_staging;
    ComPtr<ID3D11RenderTargetView>    m_rtv;
    ComPtr<ID3D11RenderTargetView>    m_rtvSrgb;
    ComPtr<ID3D11DepthStencilView>    m_dsv;
    ComPtr<ID3D11ShaderResourceView>  m_srv;
    ComPtr<ID3D11ShaderResourceView>  m_blitSrv;
    D9CubeTexture*                    m_cubeOwner{};
    class D9Texture*                  m_autogenOwner{};
    IUnknown*                         m_container;

    std::atomic<ULONG> m_refCount{ 1 };

    std::vector<uint8_t> m_shadow;
    bool                  m_shadowValid{};
    RECT                  m_dirtyRect{};
    bool                  m_hasDirty{};

    bool      m_locked{};
    RECT      m_lockedRect{};
    DWORD     m_lockFlags{};

    void*     m_rb10Bits{};
    UINT      m_rb10Pitch{};

    std::vector<uint8_t> m_lumScratch;
    void*     m_lumMapped{};
    UINT      m_lumMappedPitch{};

    HDC       m_hdc{};
    HBITMAP   m_hDib{};
    HDC       m_hdcMem{};
    void*     m_dibBits{};
    D3D11_MAPPED_SUBRESOURCE m_dcMapped{};
};

}

#endif
