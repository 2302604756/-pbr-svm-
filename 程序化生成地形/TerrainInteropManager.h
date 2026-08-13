#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <map>
#include <cmath>

using Microsoft::WRL::ComPtr;

// 保持与 CUDA 代码中的结构体完全一致 (32字节对齐)
struct TerrainVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

// 外部 CUDA 函数
extern "C" void LaunchTerrainKernel(TerrainVertex* d_vertices, int gridSize, float worldOffsetX, float worldOffsetZ);
// 云层生成核函数声明（实际更新在 RenderPipeline::UpdateCloudVolume）
extern "C" void LaunchCloudGenKernel(cudaSurfaceObject_t surface, int volumeSize);

// 1. Chunk 数据实体（D3D12 共享资源 + CUDA 外部内存导入）
struct TerrainChunk {
    int logicalX = 0, logicalZ = 0;        // 区块逻辑坐标
    float worldX = 0, worldZ = 0;          // 物理起始坐标
    ComPtr<ID3D12Resource> pVertexBuffer;  // D3D12 共享显存块 (DEFAULT heap + SHARED)
    HANDLE sharedHandle = nullptr;         // 共享 NT 句柄（CreateSharedHandle）
    cudaExternalMemory_t extMem = nullptr; // CUDA 外部内存（cudaImportExternalMemory）
    void* d_vertices = nullptr;            // CUDA 映射得到的设备指针
    UINT64 allocationSize = 0;             // D3D12 实际分配大小（64KB 对齐）
    bool isActive = false;                 // 是否正在被使用
};

// ============================================================
// CUDA-D3D12 互操作（CUDA 13 方案）：
//   CUDA 13 移除了 cuda_d3d12_interop.h（cudaGraphicsD3D12* 旧 API），
//   改用通用的 cudaImportExternalMemory 导入 D3D12 共享资源：
//     D3D12: CreateCommittedResource(SHARED) → CreateSharedHandle
//     CUDA : cudaImportExternalMemory → cudaExternalMemoryGetMappedBuffer
//   CUDA 写完后用 cudaStreamSynchronize 同步；D3D12 侧通过 barrier
//   在 COMMON(写) 与 VERTEX_AND_CONSTANT_BUFFER(渲染) 之间转换。
// ============================================================
class TerrainInteropManager {
private:
    const int GRID_SIZE = 256;            // 每个 Chunk 256x256 个顶点
    const float SPACING = 1.5f;           // 顶点间距
    const float CHUNK_SIZE = (GRID_SIZE - 1) * SPACING; // 单个 Chunk 的物理边长
    const int VIEW_DISTANCE = 2;          // 渲染半径 (2 代表 5x5 = 25 个区块)
    const int POOL_SIZE = 36;             // 显存池容量 (必须大于渲染区块数)
    int m_verticesPerChunk = 0;

    std::vector<TerrainChunk> m_chunkPool;                 // 物理显存池
    std::map<std::pair<int, int>, TerrainChunk*> m_activeChunks; // 活跃区块字典

    static UINT64 AlignUp(UINT64 v, UINT64 align) { return (v + align - 1) & ~(align - 1); }

    // 辅助函数：从池子里找一块空闲的显存
    TerrainChunk* GetFreeChunk() {
        for (auto& chunk : m_chunkPool) {
            if (!chunk.isActive) return &chunk;
        }
        return nullptr; // 池子满了！
    }

public:
    TerrainInteropManager() = default;
    ~TerrainInteropManager() { Cleanup(); }

    // ==========================================
    // 1. 初始化系统：一次性申请所有显存并导入 CUDA
    // ==========================================
    // 选择与 D3D12 适配器（LUID）匹配的 CUDA 设备。
    // CUDA 13 移除了 cudaD3D12GetDevices，改用 cudaDeviceProp.luid 与 DXGI 适配器 LUID 匹配
    static bool SetCudaDeviceForD3D12(const LUID& adapterLuid) {
        int count = 0;
        cudaGetDeviceCount(&count);
        for (int i = 0; i < count; ++i) {
            cudaDeviceProp prop;
            cudaGetDeviceProperties(&prop, i);
            bool match = true;
            for (int b = 0; b < 8; ++b) {
                BYTE expected = (b < 4) ? (BYTE)(adapterLuid.LowPart >> (8 * b))
                                        : (BYTE)(adapterLuid.HighPart >> (8 * (b - 4)));
                if (prop.luid[b] != expected) { match = false; break; }
            }
            if (match) { cudaSetDevice(i); return true; }
        }
        // 兜底：没匹配到就用设备 0
        cudaSetDevice(0);
        return false;
    }

    bool Initialize(ID3D12Device* d3dDevice, const LUID& adapterLuid) {
        m_verticesPerChunk = (GRID_SIZE - 1) * (GRID_SIZE - 1) * 6;  // 正方形数量 * 2 三角形 * 3 顶点
        const UINT64 dataSize = (UINT64)sizeof(TerrainVertex) * m_verticesPerChunk;
        SetCudaDeviceForD3D12(adapterLuid);

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = AlignUp(dataSize, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT); // 共享资源需 64KB 对齐
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        m_chunkPool.resize(POOL_SIZE);
        for (int i = 0; i < POOL_SIZE; ++i) {
            auto& chunk = m_chunkPool[i];

            // D3D12 共享顶点缓冲（初始 COMMON 状态）
            if (FAILED(d3dDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&chunk.pVertexBuffer)))) return false;
            // 用 D3D12 实际分配大小（CUDA 导入严格要求匹配）
            D3D12_RESOURCE_ALLOCATION_INFO ai = d3dDevice->GetResourceAllocationInfo(0, 1, &desc);
            chunk.allocationSize = ai.SizeInBytes;

            // 导出共享句柄
            if (FAILED(d3dDevice->CreateSharedHandle(chunk.pVertexBuffer.Get(), nullptr, GENERIC_ALL,
                    nullptr, &chunk.sharedHandle))) return false;

            // CUDA 导入外部内存
            cudaExternalMemoryHandleDesc hd = {};
            hd.type = cudaExternalMemoryHandleTypeD3D12Resource;
            hd.handle.win32.handle = (void*)chunk.sharedHandle;
            hd.size = chunk.allocationSize;
            hd.flags = cudaExternalMemoryDedicated;   // D3D12Resource 类型必须设置（CUDA 13 要求）
            cudaError_t cerr = cudaImportExternalMemory(&chunk.extMem, &hd);
            if (cerr != cudaSuccess) return false;

            cudaExternalMemoryBufferDesc bd = {};
            bd.offset = 0;
            bd.size = chunk.allocationSize;
            cerr = cudaExternalMemoryGetMappedBuffer(&chunk.d_vertices, chunk.extMem, &bd);
            if (cerr != cudaSuccess) return false;

            chunk.isActive = false;
        }
        return true;
    }

    // ==========================================
    // 1.5 批量资源状态转换（由 main 在 CUDA 更新前/渲染前调用）
    // ==========================================
    void ApplyBarriers(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        for (auto& chunk : m_chunkPool) {
            if (!chunk.pVertexBuffer) continue;
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = chunk.pVertexBuffer.Get();
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
        }
    }

    // ==========================================
    // 2. 调度系统：回收与重生 (核心对象池逻辑)
    // ==========================================
    void Update(float playerWorldX, float playerWorldZ) {
        // 计算玩家当前踩在哪一块逻辑坐标上
        int pCX = (int)floor(playerWorldX / CHUNK_SIZE);
        int pCZ = (int)floor(playerWorldZ / CHUNK_SIZE);

        // --- 阶段 A：回收 (把视野外的 Chunk 踢回对象池) ---
        for (auto it = m_activeChunks.begin(); it != m_activeChunks.end(); ) {
            TerrainChunk* chunk = it->second;
            if (abs(chunk->logicalX - pCX) > VIEW_DISTANCE || abs(chunk->logicalZ - pCZ) > VIEW_DISTANCE) {
                chunk->isActive = false; // 标记为空闲
                it = m_activeChunks.erase(it); // 从活跃字典中移除
            }
            else {
                ++it;
            }
        }

        // --- 阶段 B：重生 (为视野内的新领地呼叫 CUDA) ---
        for (int x = pCX - VIEW_DISTANCE; x <= pCX + VIEW_DISTANCE; ++x) {
            for (int z = pCZ - VIEW_DISTANCE; z <= pCZ + VIEW_DISTANCE; ++z) {
                // 如果这个坐标还没有被加载
                if (m_activeChunks.find({ x, z }) == m_activeChunks.end()) {
                    TerrainChunk* chunk = GetFreeChunk();
                    if (chunk) {
                        // 赋予它新的使命
                        chunk->logicalX = x;
                        chunk->logicalZ = z;
                        chunk->worldX = x * CHUNK_SIZE;
                        chunk->worldZ = z * CHUNK_SIZE;
                        chunk->isActive = true;
                        m_activeChunks[{x, z}] = chunk;

                        // 呼叫 CUDA 重新填充这块显存（外部内存指针直接写，无需 map/unmap）
                        LaunchTerrainKernel((TerrainVertex*)chunk->d_vertices, GRID_SIZE, chunk->worldX, chunk->worldZ);
                        cudaStreamSynchronize(0); // 等 CUDA 写完，D3D12 才能读
                    }
                }
            }
        }
    }

    // 活跃区块数量（验证用）
    int GetActiveChunkCount() const { return (int)m_activeChunks.size(); }

    // ==========================================
    // 3. 渲染命令提交：把周围的区块全画出来
    // ==========================================
    void Render(ID3D12GraphicsCommandList* cl) {
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.StrideInBytes = sizeof(TerrainVertex);
        vbv.SizeInBytes = (UINT)((UINT64)sizeof(TerrainVertex) * m_verticesPerChunk);

        // 遍历所有活着的区块，依次绘制（拓扑类型由 PSO 决定）
        for (auto const& [coord, chunk] : m_activeChunks) {
            vbv.BufferLocation = chunk->pVertexBuffer->GetGPUVirtualAddress();
            cl->IASetVertexBuffers(0, 1, &vbv);
            cl->DrawInstanced(m_verticesPerChunk, 1, 0, 0);
        }
    }

    // ==========================================
    // 4. 打扫战场
    // ==========================================
    void Cleanup() {
        for (auto& chunk : m_chunkPool) {
            if (chunk.extMem) {
                cudaDestroyExternalMemory(chunk.extMem);
                chunk.extMem = nullptr;
            }
            if (chunk.sharedHandle) {
                CloseHandle(chunk.sharedHandle);
                chunk.sharedHandle = nullptr;
            }
            chunk.d_vertices = nullptr;
            chunk.pVertexBuffer = nullptr; // ComPtr 自动释放
        }
        m_activeChunks.clear();
    }
};