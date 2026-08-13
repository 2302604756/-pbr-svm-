#pragma once
#include "GraphicsDevice.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <queue>

// 虚拟资源句柄
typedef uint32_t RGHandle;
const RGHandle RG_INVALID_HANDLE = 0;

// 虚拟纹理描述
struct RGTextureDesc {
    uint32_t Width, Height;
    DXGI_FORMAT Format;
    bool IsDepthStencil; // true: 创建 DSV+SRV, false: 创建 RTV+SRV
};

// 物理资源包装器
struct RGPhysicalResource {
    ComPtr<ID3D11Texture2D> Texture;
    ComPtr<ID3D11ShaderResourceView> SRV;
    ComPtr<ID3D11RenderTargetView> RTV;
    ComPtr<ID3D11DepthStencilView> DSV;
};

class RenderGraph;

// 注册机：在 Execute 阶段供 Pass 获取真实的 DX11 指针
class RenderGraphRegistry {
    //利用哈希表来实现虚拟纹理描述与物理资源的映射
    std::unordered_map<RGHandle, RGPhysicalResource>* m_resources;
public:
    RenderGraphRegistry(std::unordered_map<RGHandle, RGPhysicalResource>* res) : m_resources(res) {}

    ID3D11ShaderResourceView* GetSRV(RGHandle h) { return (*m_resources)[h].SRV.Get(); }
    ID3D11RenderTargetView* GetRTV(RGHandle h) { return (*m_resources)[h].RTV.Get(); }
    ID3D11DepthStencilView* GetDSV(RGHandle h) { return (*m_resources)[h].DSV.Get(); }
};

// 构建器：在 Setup 阶段供 Pass 声明依赖
class RenderGraphBuilder {
    RenderGraph* m_graph;
    uint32_t m_passIndex;
public:
    RenderGraphBuilder(RenderGraph* g, uint32_t passIdx) : m_graph(g), m_passIndex(passIdx) {}

    RGHandle CreateTexture(const std::string& name, const RGTextureDesc& desc);
    void Read(RGHandle handle);
    void Write(RGHandle handle);
};

// 图节点：Pass
struct RGPassNode {
    std::string Name;
    std::vector<RGHandle> Reads;
    std::vector<RGHandle> Writes;
    std::function<void(GraphicsDevice&, RenderGraphRegistry&)> ExecuteCallback;
    uint32_t InDegree = 0; // 用于拓扑排序
};

// 渲染图主体
class RenderGraph {
    friend class RenderGraphBuilder;
private:
    uint32_t m_nextHandle = 1;
    std::unordered_map<RGHandle, RGTextureDesc> m_virtualResources;     //虚拟资源desc
    std::unordered_map<RGHandle, std::string> m_resourceNames;          //资源名称
    std::unordered_map<RGHandle, RGPhysicalResource> m_physicalResources;//物理资源

    std::vector<RGPassNode> m_passes;//存passnode 里面有
    std::vector<uint32_t> m_sortedPassIndices;

public:
    // 导入外部物理资源（如主屏幕的 BackBuffer）
    RGHandle ImportRenderTarget(const std::string& name, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
        RGHandle h = m_nextHandle++;
        RGPhysicalResource phys;
        phys.RTV = rtv;
        phys.DSV = dsv;
        m_physicalResources[h] = phys;
        m_resourceNames[h] = name;
        return h;
    }

    // 添加 Pass：使用 Lambda 表达式实现两阶段提交
    template<typename SetupFunc, typename ExecuteFunc>
    void AddPass(const std::string& name, SetupFunc setup, ExecuteFunc execute) {
        RGPassNode node;
        node.Name = name;
        m_passes.push_back(node);
        //快速指定到当前索引
        uint32_t passIdx = m_passes.size() - 1;

        RenderGraphBuilder builder(this, passIdx);
        setup(builder); // 执行 Setup，收集依赖

        m_passes[passIdx].ExecuteCallback = execute; // 绑定执行逻辑
    }

    // 编译期：拓扑排序与物理显存分配
    void Compile(GraphicsDevice& gfx) {
        // 1. 分配所有 Setup 阶段声明的虚拟纹理
        for (const auto& pair : m_virtualResources) {
            RGHandle h = pair.first;
            const RGTextureDesc& desc = pair.second;

            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = desc.Width; texDesc.Height = desc.Height;
            texDesc.MipLevels = 1; texDesc.ArraySize = 1;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_DEFAULT;

            RGPhysicalResource phys;

            if (desc.IsDepthStencil) {
                texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
                gfx.pd3dDevice->CreateTexture2D(&texDesc, nullptr, phys.Texture.GetAddressOf());

                D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                gfx.pd3dDevice->CreateDepthStencilView(phys.Texture.Get(), &dsvDesc, phys.DSV.GetAddressOf());

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                gfx.pd3dDevice->CreateShaderResourceView(phys.Texture.Get(), &srvDesc, phys.SRV.GetAddressOf());
            }
            else {
                texDesc.Format = desc.Format;
                texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                gfx.pd3dDevice->CreateTexture2D(&texDesc, nullptr, phys.Texture.GetAddressOf());
                gfx.pd3dDevice->CreateRenderTargetView(phys.Texture.Get(), nullptr, phys.RTV.GetAddressOf());
                gfx.pd3dDevice->CreateShaderResourceView(phys.Texture.Get(), nullptr, phys.SRV.GetAddressOf());
            }
            m_physicalResources[h] = phys;
        }

        // 2. 简单的拓扑排序（基于依赖关系推导执行顺序）
        // 这里为了简化演示，假设用户的 AddPass 顺序就是合理顺序，直接顺序执行。
        // 在完整的系统中，这里会通过读取/写入关系计算 InDegree 并使用 Kahn 算法排序。
        for (uint32_t i = 0; i < m_passes.size(); ++i) {
            m_sortedPassIndices.push_back(i);
        }
    }

    // 执行期：按图逻辑发射 Draw Call，并自动解绑资源
    void Execute(GraphicsDevice& gfx) {
        RenderGraphRegistry registry(&m_physicalResources);

        for (uint32_t idx : m_sortedPassIndices) {
            auto& pass = m_passes[idx];
            
            // 执行核心渲染回调
            pass.ExecuteCallback(gfx, registry);

            // 【黑魔法：自动解绑】
            // 引擎自动清空这个 Pass 读取过的所有 SRV，防止下一个 Pass 将其作为 RTV 写入时 DX11 报错！
            ID3D11ShaderResourceView* nullSRVs[16] = { nullptr };
            gfx.pImmediateContext->PSSetShaderResources(0, pass.Reads.size(), nullSRVs);
        }
    }

private:
    // Builder 调用的内部接口
    RGHandle CreateResourceInternal(const std::string& name, const RGTextureDesc& desc) {
        RGHandle h = m_nextHandle++;
        m_virtualResources[h] = desc;
        m_resourceNames[h] = name;
        return h;
    }
};

// Builder 方法实现
inline RGHandle RenderGraphBuilder::CreateTexture(const std::string& name, const RGTextureDesc& desc) {
    return m_graph->CreateResourceInternal(name, desc);
}
inline void RenderGraphBuilder::Read(RGHandle handle) { m_graph->m_passes[m_passIndex].Reads.push_back(handle); }
inline void RenderGraphBuilder::Write(RGHandle handle) { m_graph->m_passes[m_passIndex].Writes.push_back(handle); }