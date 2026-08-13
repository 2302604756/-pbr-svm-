// d3dx12lite.h — 轻量 D3D12 辅助函数
// 说明：新版 Windows SDK (26100+) 已移除 d3dx12.h，本项目只需要其中少量辅助，
// 这里手写等价实现，避免额外依赖。
#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>

namespace d3dx12lite {

inline UINT64 AlignUp(UINT64 v, UINT64 align) { return (v + align - 1) & ~(align - 1); }

// 默认光栅化状态（等同 CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT)）
inline D3D12_RASTERIZER_DESC DefaultRasterizer() {
    D3D12_RASTERIZER_DESC d = {};
    d.FillMode = D3D12_FILL_MODE_SOLID;
    d.CullMode = D3D12_CULL_MODE_BACK;
    d.FrontCounterClockwise = FALSE;
    d.DepthBias = 0;
    d.DepthBiasClamp = 0.0f;
    d.SlopeScaledDepthBias = 0.0f;
    d.DepthClipEnable = TRUE;
    d.MultisampleEnable = FALSE;
    d.AntialiasedLineEnable = FALSE;
    d.ForcedSampleCount = 0;
    d.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return d;
}

// 默认深度模板状态（等同 CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT)）
inline D3D12_DEPTH_STENCIL_DESC DefaultDepthStencil() {
    D3D12_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    d.StencilEnable = FALSE;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    d.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.BackFace = d.FrontFace;
    return d;
}

// 默认混合状态（等同 CD3DX12_BLEND_DESC(D3D12_DEFAULT)）
inline D3D12_BLEND_DESC DefaultBlend() {
    D3D12_BLEND_DESC d = {};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i) {
        d.RenderTarget[i].BlendEnable = FALSE;
        d.RenderTarget[i].LogicOpEnable = FALSE;
        d.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
        d.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        d.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
        d.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ONE;
        d.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        d.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        d.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
        d.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return d;
}

// Shader 字节码（等同 CD3DX12_SHADER_BYTECODE）
inline D3D12_SHADER_BYTECODE ShaderBytecode(ID3DBlob* blob) {
    D3D12_SHADER_BYTECODE bc = {};
    bc.pShaderBytecode = blob ? blob->GetBufferPointer() : nullptr;
    bc.BytecodeLength = blob ? blob->GetBufferSize() : 0;
    return bc;
}

// 2D 纹理上传：stb 数据（每行 width*4 字节）→ upload buffer（行距 256B 对齐）→ CopyTextureRegion
// 返回实际使用的上传缓冲大小
inline UINT64 UploadTexture2D(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                              ID3D12Resource* dest, ID3D12Resource* upload,
                              const void* pData, UINT width, UINT height,
                              DXGI_FORMAT format) {
    const UINT64 srcRowPitch = (UINT64)width * 4;
    const UINT64 dstRowPitch = AlignUp(srcRowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 total = dstRowPitch * height;

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    const unsigned char* src = (const unsigned char*)pData;
    unsigned char* dst = (unsigned char*)mapped;
    for (UINT y = 0; y < height; ++y) {
        memcpy(dst + y * dstRowPitch, src + y * srcRowPitch, srcRowPitch);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Width = width;
    srcLoc.PlacedFootprint.Footprint.Height = height;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.Format = format;
    srcLoc.PlacedFootprint.Footprint.RowPitch = (UINT)dstRowPitch;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = dest;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    return total;
}

} // namespace d3dx12lite
