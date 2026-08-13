#pragma once
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>

using Microsoft::WRL::ComPtr;

typedef uint32_t RGHandle;
const RGHandle RG_INVALID_HANDLE = 0;

// 1. 虚拟纹理描述符
struct RGTextureDesc {
    uint32_t Width, Height;
    DXGI_FORMAT Format;
    bool IsDepthStencil;
};

// 2. DX12 物理资源包装器
struct RGPhysicalResource {
    ComPtr<ID3D12Resource> Resource;
    D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;

    // 预创建好的 CPU 描述符句柄 (来自对应的 Descriptor Heap)
    D3D12_CPU_DESCRIPTOR_HANDLE SRVHandle{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE DSVHandle{ 0 };
};

// 3. DX12 注册机：Execute 阶段供 Pass 获取 SRV/RTV/DSV 的 CPU 句柄
class RenderGraphRegistry {
    std::unordered_map<RGHandle, RGPhysicalResource>* m_resources;
public:
    RenderGraphRegistry(std::unordered_map<RGHandle, RGPhysicalResource>* res) : m_resources(res) {}

    ID3D12Resource* GetResource(RGHandle h) { return (*m_resources)[h].Resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(RGHandle h) { return (*m_resources)[h].SRVHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(RGHandle h) { return (*m_resources)[h].RTVHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV(RGHandle h) { return (*m_resources)[h].DSVHandle; }
};

class RenderGraph;

// 4. 构建器：Setup 阶段收集 Pass 的资源依赖与期望状态
class RenderGraphBuilder {
    RenderGraph* m_graph;
    uint32_t m_passIndex;
public:
    RenderGraphBuilder(RenderGraph* g, uint32_t passIdx) : m_graph(g), m_passIndex(passIdx) {}

    RGHandle CreateTexture(const std::string& name, const RGTextureDesc& desc);

    // DX12 核心扩展：明确指定读取/写入时的期望 State
    void Read(RGHandle handle, D3D12_RESOURCE_STATES expectedState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    void Write(RGHandle handle, D3D12_RESOURCE_STATES expectedState = D3D12_RESOURCE_STATE_RENDER_TARGET);
};

// 5. 资源访问声明节点
struct RGResourceAccess {
    RGHandle Handle;
    D3D12_RESOURCE_STATES DesiredState;
};

// 6. 图节点：Pass
struct RGPassNode {
    std::string Name;
    std::vector<RGResourceAccess> Accesses; // 记录所有的资源访问偏好
    std::function<void(ID3D12GraphicsCommandList*, RenderGraphRegistry&)> ExecuteCallback;
};

// 7. DX12 渲染图主体
class RenderGraph {
    friend class RenderGraphBuilder;
private:
    uint32_t m_nextHandle = 1;
    std::unordered_map<RGHandle, RGTextureDesc> m_virtualResources;
    std::unordered_map<RGHandle, std::string> m_resourceNames;
    std::unordered_map<RGHandle, RGPhysicalResource> m_physicalResources;

    std::vector<RGPassNode> m_passes;
    std::vector<uint32_t> m_sortedPassIndices;

    // 编译期推导出的屏障批次：m_passBarriers[sortedPassIdx] 存该 Pass 执行前需要提交的 Barrier 列表
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> m_passBarriers;

public:
    // 导入外部物理资源（如 SwapChain 当前帧 BackBuffer）
    RGHandle ImportRenderTarget(const std::string& name, ID3D12Resource* pRes, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, D3D12_RESOURCE_STATES initialState) {
        RGHandle h = m_nextHandle++;
        RGPhysicalResource phys;
        phys.Resource = pRes;
        phys.RTVHandle = rtvHandle;
        phys.CurrentState = initialState;
        m_physicalResources[h] = phys;
        m_resourceNames[h] = name;
        return h;
    }

    template<typename SetupFunc, typename ExecuteFunc>
    void AddPass(const std::string& name, SetupFunc setup, ExecuteFunc execute) {
        RGPassNode node;
        node.Name = name;
        m_passes.push_back(node);
        uint32_t passIdx = static_cast<uint32_t>(m_passes.size() - 1);

        RenderGraphBuilder builder(this, passIdx);
        setup(builder);

        m_passes[passIdx].ExecuteCallback = execute;
    }

    // 编译期：物理资源创建与自动 Barrier 推导
    void Compile(ID3D12Device* device, ID3D12DescriptorHeap* rtvHeap, ID3D12DescriptorHeap* dsvHeap, ID3D12DescriptorHeap* srvHeap, UINT descriptorSizeRTV, UINT descriptorSizeDSV, UINT descriptorSizeSRV) {
        // 1. 物理 VRAM 分配 (Committed Resource 示范，工业级可在此替换为 Placed Resource 堆复用)
        UINT rtvOffset = 0, dsvOffset = 0, srvOffset = 0;

        for (const auto& pair : m_virtualResources) {
            RGHandle h = pair.first;
            const RGTextureDesc& desc = pair.second;

            // 跳过已导入的外部资源
            if (m_physicalResources.find(h) != m_physicalResources.end()) continue;

            D3D12_RESOURCE_DESC resDesc = {};
            resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resDesc.Width = desc.Width; resDesc.Height = desc.Height;
            resDesc.DepthOrArraySize = 1; resDesc.MipLevels = 1;
            resDesc.SampleDesc.Count = 1;
            resDesc.Flags = desc.IsDepthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearValue = {};
            if (desc.IsDepthStencil) {
                resDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                clearValue.Format = DXGI_FORMAT_D32_FLOAT;
                clearValue.DepthStencil.Depth = 1.0f;
            }
            else {
                resDesc.Format = desc.Format;
                clearValue.Format = desc.Format;
                clearValue.Color[0] = 0.0f; clearValue.Color[1] = 0.0f; clearValue.Color[2] = 0.0f; clearValue.Color[3] = 1.0f;
            }

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            RGPhysicalResource phys;
            phys.CurrentState = desc.IsDepthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

            device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                phys.CurrentState, &clearValue,
                IID_PPV_ARGS(phys.Resource.GetAddressOf())
            );

            // 简单切分描述符句柄 (生产环境建议接入专用 DescriptorAllocator)
            if (desc.IsDepthStencil) {
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc = {};
                dsvViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

                phys.DSVHandle.ptr = dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (dsvOffset++ * descriptorSizeDSV);
                device->CreateDepthStencilView(phys.Resource.Get(), &dsvViewDesc, phys.DSVHandle);

                D3D12_SHADER_RESOURCE_VIEW_DESC srvViewDesc = {};
                srvViewDesc.Format = DXGI_FORMAT_R32_FLOAT;
                srvViewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvViewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvViewDesc.Texture2D.MipLevels = 1;

                phys.SRVHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (srvOffset++ * descriptorSizeSRV);
                device->CreateShaderResourceView(phys.Resource.Get(), &srvViewDesc, phys.SRVHandle);
            }
            else {
                phys.RTVHandle.ptr = rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (rtvOffset++ * descriptorSizeRTV);
                device->CreateRenderTargetView(phys.Resource.Get(), nullptr, phys.RTVHandle);

                phys.SRVHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (srvOffset++ * descriptorSizeSRV);
                device->CreateShaderResourceView(phys.Resource.Get(), nullptr, phys.SRVHandle);
            }

            m_physicalResources[h] = phys;
        }

        // 2. 拓扑排序 (默认按添加顺序)
        m_sortedPassIndices.clear();
        for (uint32_t i = 0; i < m_passes.size(); ++i) m_sortedPassIndices.push_back(i);

        // 3. 核心：自动 Barrier 模拟与推导生成
        m_passBarriers.clear();
        m_passBarriers.resize(m_sortedPassIndices.size());

        // 跟踪每个物理资源的全局模拟状态
        std::unordered_map<RGHandle, D3D12_RESOURCE_STATES> stateTracker;
        for (const auto& [handle, phys] : m_physicalResources) {
            stateTracker[handle] = phys.CurrentState;
        }

        for (size_t sortedIdx = 0; sortedIdx < m_sortedPassIndices.size(); ++sortedIdx) {
            uint32_t passIdx = m_sortedPassIndices[sortedIdx];
            auto& pass = m_passes[passIdx];

            for (const auto& access : pass.Accesses) {
                RGHandle h = access.Handle;
                D3D12_RESOURCE_STATES desiredState = access.DesiredState;
                D3D12_RESOURCE_STATES currentState = stateTracker[h];

                if (currentState != desiredState) {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = m_physicalResources[h].Resource.Get();
                    barrier.Transition.StateBefore = currentState;
                    barrier.Transition.StateAfter = desiredState;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    m_passBarriers[sortedIdx].push_back(barrier);

                    // 更新跟踪器的状态，供下一个 Pass 继续推导
                    stateTracker[h] = desiredState;
                }
            }
        }
    }

    // 运行时：按图自动发射 Barrier 并执行 Command List 录制
    void Execute(ID3D12GraphicsCommandList* cmdList) {
        RenderGraphRegistry registry(&m_physicalResources);

        for (size_t sortedIdx = 0; sortedIdx < m_sortedPassIndices.size(); ++sortedIdx) {
            uint32_t passIdx = m_sortedPassIndices[sortedIdx];
            auto& pass = m_passes[passIdx];

            // 1. 自动批处理刷入当前 Pass 所需的所有 Transition Barriers
            const auto& barriers = m_passBarriers[sortedIdx];
            if (!barriers.empty()) {
                cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            }

            // 2. 纯净录制该 Pass 的命令
            pass.ExecuteCallback(cmdList, registry);
        }
    }

private:
    RGHandle CreateResourceInternal(const std::string& name, const RGTextureDesc& desc) {
        RGHandle h = m_nextHandle++;
        m_virtualResources[h] = desc;
        m_resourceNames[h] = name;
        return h;
    }
};

inline RGHandle RenderGraphBuilder::CreateTexture(const std::string& name, const RGTextureDesc& desc) {
    return m_graph->CreateResourceInternal(name, desc);
}
inline void RenderGraphBuilder::Read(RGHandle handle, D3D12_RESOURCE_STATES expectedState) {
    m_graph->m_passes[m_passIndex].Accesses.push_back({ handle, expectedState });
}
inline void RenderGraphBuilder::Write(RGHandle handle, D3D12_RESOURCE_STATES expectedState) {
    m_graph->m_passes[m_passIndex].Accesses.push_back({ handle, expectedState });
}



RenderGraph rg;

// 1. 导入 SwapChain 的渲染目标
RGHandle backBuffer = rg.ImportRenderTarget("BackBuffer", currentSwapChainResource, currentRTVHandle, D3D12_RESOURCE_STATE_PRESENT);

// 2. Main Pass
RGHandle sceneColor;
rg.AddPass("MainPass",
    [&](RenderGraphBuilder& builder) {
        sceneColor = builder.CreateTexture("SceneColor", { 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, false });

        // 显式声明：在此 Pass 中，SceneColor 要作为 RenderTarget 写入
        builder.Write(sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    },
    [=](ID3D12GraphicsCommandList* cmdList, RenderGraphRegistry& registry) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(sceneColor);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // cmdList->DrawIndexedInstanced(...);
    }
);

// 3. PostProcess Pass
rg.AddPass("PostProcessPass",
    [&](RenderGraphBuilder& builder) {
        // 显式声明：在此 Pass 中，SceneColor 转换为 Pixel Shader 采样源
        builder.Read(sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        builder.Write(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    },
    [=](ID3D12GraphicsCommandList* cmdList, RenderGraphRegistry& registry) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(backBuffer);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // 绑定 SceneColor 的 SRV 并画全屏 Fullscreen Triangle...
    }
);

// 4. 编译与发射
rg.Compile(device, rtvHeap, dsvHeap, srvHeap, rtvSize, dsvSize, srvSize);
rg.Execute(commandList); // 自动在 Pass 之间吐出正确的 ResourceBarrier！