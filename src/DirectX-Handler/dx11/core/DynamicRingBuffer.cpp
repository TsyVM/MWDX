// Sub-allocates one large dynamic buffer for per-draw streaming data.
//
// The correctness of this rests entirely on which map flag is used. Writing
// into a region the GPU may still be reading corrupts a draw already queued;
// waiting for the GPU on every allocation would serialise the frame. D3D11
// resolves this with two promises the caller can make:
//
//   NO_OVERWRITE  "I am only writing regions you have finished with." The
//                 driver does not wait. Valid while the cursor is advancing
//                 into untouched space.
//   DISCARD       "Give me fresh storage, the old contents are dead." The
//                 driver hands back new memory and orphans the old, which is
//                 the only safe option when the cursor wraps.
//
// So allocations advance the cursor under NO_OVERWRITE and, on running out of
// room, discard once and start again from zero. Callers are told when that
// happened, because a discard invalidates every offset previously handed out.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/DynamicRingBuffer.h>
#include <utility>

namespace dx9to11 {

HRESULT DynamicRingBuffer::Create(
    ID3D11Device1*      pDevice,
    UINT                sizeInBytes,
    UINT                bindFlags,
    DynamicRingBuffer** ppOut) noexcept
{
    if (!pDevice || !ppOut || sizeInBytes == 0)
        return E_INVALIDARG;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth      = sizeInBytes;
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.BindFlags      = bindFlags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = pDevice->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr))
        return hr;

    auto* ring        = new DynamicRingBuffer();
    ring->m_buffer     = std::move(buffer);
    ring->m_capacity   = sizeInBytes;
    ring->m_cursor     = 0;
    *ppOut = ring;
    return S_OK;
}

HRESULT DynamicRingBuffer::Map(
    ID3D11DeviceContext1* pContext,
    UINT                  sizeInBytes,
    bool                  discard,
    RingAllocation*       pOut) noexcept
{
    if (!pContext || !pOut || sizeInBytes == 0 || sizeInBytes > m_capacity)
        return E_INVALIDARG;

    AcquireSRWLockExclusive(&m_lock);

    bool wrapped = discard;
    if (!wrapped && (m_cursor + sizeInBytes > m_capacity)) {
        wrapped = true;
    }
    if (wrapped) {
        m_cursor = 0;
    }

    const D3D11_MAP mapType = wrapped ? D3D11_MAP_WRITE_DISCARD
                                       : D3D11_MAP_WRITE_NO_OVERWRITE;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = pContext->Map(m_buffer.Get(), 0, mapType, 0, &mapped);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&m_lock);
        return hr;
    }

    pOut->offsetInBytes = m_cursor;
    pOut->wasDiscard     = wrapped;
    pOut->pData          = static_cast<uint8_t*>(mapped.pData) + m_cursor;

    m_cursor += sizeInBytes;

    return S_OK;
}

void DynamicRingBuffer::Unmap(ID3D11DeviceContext1* pContext) noexcept
{
    if (pContext) {
        pContext->Unmap(m_buffer.Get(), 0);
    }
    ReleaseSRWLockExclusive(&m_lock);
}

}
