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
    UINT SRVOffset = 0;   // SRV 在 SRV 堆中的偏移（用于计算 GPU 句柄）
};

// 3. DX12 注册机：Execute 阶段供 Pass 获取 SRV/RTV/DSV 句柄
class RenderGraphRegistry {
    std::unordered_map<RGHandle, RGPhysicalResource>* m_resources;
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvHeapStartGPU{};
    UINT m_srvDescriptorSize = 0;
public:
    RenderGraphRegistry(std::unordered_map<RGHandle, RGPhysicalResource>* res,
                        D3D12_GPU_DESCRIPTOR_HANDLE srvStart, UINT srvSize)
        : m_resources(res), m_srvHeapStartGPU(srvStart), m_srvDescriptorSize(srvSize) {}

    ID3D12Resource* GetResource(RGHandle h) { return (*m_resources)[h].Resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRV(RGHandle h) { return (*m_resources)[h].SRVHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(RGHandle h) { return (*m_resources)[h].RTVHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV(RGHandle h) { return (*m_resources)[h].DSVHandle; }
    // DX12 专用：把资源的 SRV 转成 GPU 可见句柄（绑定 Descriptor Table 用）
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVGPU(RGHandle h) {
        D3D12_GPU_DESCRIPTOR_HANDLE g;
        g.ptr = m_srvHeapStartGPU.ptr + (UINT64)(*m_resources)[h].SRVOffset * m_srvDescriptorSize;
        return g;
    }
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

// 6. 编译期推导出的 Barrier：Handle 用于在 Execute 时解析资源指针
//    （交换链 backbuffer 每帧变化，pResource 必须在执行期填充）
struct RGBarrier {
    RGHandle Handle;
    D3D12_RESOURCE_BARRIER Barrier;
};

// 7. 图节点：Pass
struct RGPassNode {
    std::string Name;
    std::vector<RGResourceAccess> Accesses; // 记录所有的资源访问偏好
    std::function<void(ID3D12GraphicsCommandList*, RenderGraphRegistry&)> ExecuteCallback;
};

// 8. DX12 渲染图主体
class RenderGraph {
    friend class RenderGraphBuilder;
private:
    uint32_t m_nextHandle = 1;
    std::unordered_map<RGHandle, RGTextureDesc> m_virtualResources;
    std::unordered_map<RGHandle, std::string> m_resourceNames;
    std::unordered_map<RGHandle, RGPhysicalResource> m_physicalResources;

    std::vector<RGPassNode> m_passes;
    std::vector<uint32_t> m_sortedPassIndices;

    // 编译期推导出的屏障批次：m_passBarriers[sortedPassIdx] 存该 Pass 执行前需要提交的 Barrier
    std::vector<std::vector<RGBarrier>> m_passBarriers;

    // SRV 堆信息（Execute 时计算 GPU 句柄）
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvHeapStartGPU{};
    UINT m_srvDescriptorSize = 0;

public:
    // 导入外部物理资源（如 SwapChain 当前帧 BackBuffer）
    RGHandle ImportRenderTarget(const std::string& name, ID3D12Resource* pRes,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
                                D3D12_RESOURCE_STATES initialState) {
        RGHandle h = m_nextHandle++;
        RGPhysicalResource phys;
        phys.Resource = pRes;
        phys.RTVHandle = rtvHandle;
        phys.CurrentState = initialState;
        m_physicalResources[h] = phys;
        m_resourceNames[h] = name;
        return h;
    }

    // 导入外部深度资源（如主屏幕的 DepthBuffer，供多个 Pass 按不同状态访问）
    RGHandle ImportDepthTarget(const std::string& name, ID3D12Resource* pRes,
                               D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
                               D3D12_RESOURCE_STATES initialState) {
        RGHandle h = m_nextHandle++;
        RGPhysicalResource phys;
        phys.Resource = pRes;
        phys.DSVHandle = dsvHandle;
        phys.CurrentState = initialState;
        m_physicalResources[h] = phys;
        m_resourceNames[h] = name;
        return h;
    }

    // 每帧更新导入资源（SwapChain 每帧轮换 backbuffer：指针与 RTV 都要更新）
    void UpdateImportedResource(RGHandle h, ID3D12Resource* pRes, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {}) {
        m_physicalResources[h].Resource = pRes;
        if (rtvHandle.ptr) m_physicalResources[h].RTVHandle = rtvHandle;
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
    // base offsets：描述符堆中已由外部占用的偏移（backbuffer RTV、主深度 DSV、材质 SRV 等）
    void Compile(ID3D12Device* device,
                 ID3D12DescriptorHeap* rtvHeap, ID3D12DescriptorHeap* dsvHeap, ID3D12DescriptorHeap* srvHeap,
                 UINT descriptorSizeRTV, UINT descriptorSizeDSV, UINT descriptorSizeSRV,
                 UINT rtvBaseOffset = 0, UINT dsvBaseOffset = 0, UINT srvBaseOffset = 0) {
        m_srvHeapStartGPU = srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_srvDescriptorSize = descriptorSizeSRV;

        // 1. 物理 VRAM 分配（按 handle 升序，保证描述符偏移确定）
        std::vector<RGHandle> handles;
        for (const auto& pair : m_virtualResources) handles.push_back(pair.first);
        std::sort(handles.begin(), handles.end());

        UINT rtvOffset = rtvBaseOffset, dsvOffset = dsvBaseOffset, srvOffset = srvBaseOffset;

        for (RGHandle h : handles) {
            const RGTextureDesc& desc = m_virtualResources[h];

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

            if (FAILED(device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                    phys.CurrentState, &clearValue,
                    IID_PPV_ARGS(phys.Resource.GetAddressOf())))) {
                continue;
            }

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

                phys.SRVHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (srvOffset * descriptorSizeSRV);
                phys.SRVOffset = srvOffset++;
                device->CreateShaderResourceView(phys.Resource.Get(), &srvViewDesc, phys.SRVHandle);
            }
            else {
                phys.RTVHandle.ptr = rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (rtvOffset++ * descriptorSizeRTV);
                device->CreateRenderTargetView(phys.Resource.Get(), nullptr, phys.RTVHandle);

                phys.SRVHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (srvOffset * descriptorSizeSRV);
                phys.SRVOffset = srvOffset++;
                device->CreateShaderResourceView(phys.Resource.Get(), nullptr, phys.SRVHandle);
            }

            m_physicalResources[h] = phys;
        }

        // 2. 拓扑排序 (默认按添加顺序；完整实现可在此接入 Kahn 算法)
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
                D3D12_RESOURCE_STATES desiredState = access.DesiredState;   // 从 Accesses 中读取想要的状态
                D3D12_RESOURCE_STATES currentState = stateTracker[h];       // 资源上一次用完以后的状态

                if (currentState != desiredState) {
                    RGBarrier rb;
                    rb.Handle = h;
                    rb.Barrier = {};
                    rb.Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    // pResource 留空：交换链 backbuffer 每帧变化，必须在 Execute 时解析
                    rb.Barrier.Transition.StateBefore = currentState;
                    rb.Barrier.Transition.StateAfter = desiredState;
                    rb.Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

                    m_passBarriers[sortedIdx].push_back(rb);

                    // 更新跟踪器的状态，供下一个 Pass 继续推导
                    stateTracker[h] = desiredState;
                }
            }
        }
    }

    // 运行时：按图自动发射 Barrier 并执行 Command List 录制
    void Execute(ID3D12GraphicsCommandList* cmdList) {
        RenderGraphRegistry registry(&m_physicalResources, m_srvHeapStartGPU, m_srvDescriptorSize);

        for (size_t sortedIdx = 0; sortedIdx < m_sortedPassIndices.size(); ++sortedIdx) {
            uint32_t passIdx = m_sortedPassIndices[sortedIdx];
            auto& pass = m_passes[passIdx];

            // 1. 自动批处理刷入当前 Pass 所需的所有 Transition Barriers（延迟解析资源指针）
            const auto& barriers = m_passBarriers[sortedIdx];
            if (!barriers.empty()) {
                thread_local std::vector<D3D12_RESOURCE_BARRIER> resolved;
                resolved.clear();
                resolved.reserve(barriers.size());
                for (const auto& rb : barriers) {
                    D3D12_RESOURCE_BARRIER b = rb.Barrier;
                    b.Transition.pResource = m_physicalResources[rb.Handle].Resource.Get();
                    resolved.push_back(b);
                }
                cmdList->ResourceBarrier(static_cast<UINT>(resolved.size()), resolved.data());
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

// Builder 方法实现
inline RGHandle RenderGraphBuilder::CreateTexture(const std::string& name, const RGTextureDesc& desc) {
    return m_graph->CreateResourceInternal(name, desc);
}
inline void RenderGraphBuilder::Read(RGHandle handle, D3D12_RESOURCE_STATES expectedState) {
    m_graph->m_passes[m_passIndex].Accesses.push_back({ handle, expectedState });
}
inline void RenderGraphBuilder::Write(RGHandle handle, D3D12_RESOURCE_STATES expectedState) {
    m_graph->m_passes[m_passIndex].Accesses.push_back({ handle, expectedState });
}