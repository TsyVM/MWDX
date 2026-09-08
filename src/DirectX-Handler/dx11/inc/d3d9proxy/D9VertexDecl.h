// IDirect3DVertexDeclaration9 -- how to read vertices out of the bound streams.
//
// This holds the D3D9 element array and nothing else. It is deliberately not
// translated into an ID3D11InputLayout here, because D3D11 validates a layout
// against the signature of the shader it will be used with: the same
// declaration paired with two different vertex shaders needs two different
// layouts. The pairing is only known at draw time, so the translation happens
// there and is cached on the (declaration, shader) pair.
//
// The element array is copied rather than referenced. D3D9's contract lets the
// caller free its array as soon as the declaration is created.

#pragma once

#ifndef DX9TO11_D9_VERTEX_DECL_H
#define DX9TO11_D9_VERTEX_DECL_H

#include <d3d9.h>
#include <vector>

namespace dx9to11 {

class D9Device;

class D9VertexDecl final : public IDirect3DVertexDeclaration9 {
public:

    D9VertexDecl(D9Device* pDevice, const D3DVERTEXELEMENT9* pElements) noexcept;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* pElements,
                                              UINT* pNumElements) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    [[nodiscard]] const D3DVERTEXELEMENT9* Elements()  const noexcept { return m_elements.data(); }
    [[nodiscard]] UINT                     NumElements() const noexcept { return static_cast<UINT>(m_elements.size()); }

private:
    ~D9VertexDecl() = default;

    LONG                            m_refCount{ 1 };
    D9Device*                       m_device;
    std::vector<D3DVERTEXELEMENT9>  m_elements;
};

}

#endif
