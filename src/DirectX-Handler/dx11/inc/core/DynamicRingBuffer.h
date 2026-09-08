#pragma once

#ifndef DX9TO11_DYNAMIC_RING_BUFFER_H
#define DX9TO11_DYNAMIC_RING_BUFFER_H

#include <d3d11_1.h>
#include <wrl/client.h>
#include <synchapi.h>
#include <cstdint>

// Streaming buffer for per-draw vertex and index data.
//
// D3D9's DrawPrimitiveUP family takes vertices straight from a user pointer.
// D3D11 has no equivalent — the data must be in a buffer — and Most Wanted
// routes its entire front end, menus and HUD through those calls, so this is
// on the hot path for real frames.
//
// A fresh buffer per draw would be unusable, so one large DYNAMIC buffer is
// sub-allocated with a moving cursor. The distinction that makes this correct
// is which Map flag is used: WRITE_NO_OVERWRITE promises the GPU that the
// region being written is not one it may still be reading, which lets it
// continue without a stall; WRITE_DISCARD tells the driver to hand back fresh
// storage and orphan the old, which is the only safe thing to do when the
// cursor wraps and would otherwise overwrite data a queued draw still needs.
//
// So allocations use NO_OVERWRITE until the cursor runs out of room, then
// DISCARD once and reset to the start. `wasDiscard` reports which happened,
// because a discard invalidates every offset previously handed out and any
// caller holding one must re-bind.

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

struct RingAllocation {
    UINT       offsetInBytes = 0;
    bool       wasDiscard    = false;
    void*      pData         = nullptr;
};

class DynamicRingBuffer {
public:

    static HRESULT Create(
        ID3D11Device1*       pDevice,
        UINT                 sizeInBytes,
        UINT                 bindFlags,
        DynamicRingBuffer**  ppOut) noexcept;

    [[nodiscard]] ID3D11Buffer* Buffer() const noexcept { return m_buffer.Get(); }
    [[nodiscard]] UINT          Capacity() const noexcept { return m_capacity; }

    HRESULT Map(
        ID3D11DeviceContext1* pContext,
        UINT                  sizeInBytes,
        bool                  discard,
        RingAllocation*       pOut) noexcept;

    void Unmap(ID3D11DeviceContext1* pContext) noexcept;

private:
    DynamicRingBuffer() = default;

    ComPtr<ID3D11Buffer> m_buffer;
    UINT                 m_capacity = 0;
    UINT                 m_cursor   = 0;
    SRWLOCK              m_lock     = SRWLOCK_INIT;
};

}

#endif
