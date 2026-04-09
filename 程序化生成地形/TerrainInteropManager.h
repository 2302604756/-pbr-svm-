#pragma once

#include <d3d11.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <iostream>
#include <vector>
#include <map>
#include <cmath>

// 保持与 CUDA 代码中的结构体完全一致 (32字节对齐)
struct TerrainVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

// 1. Chunk 数据实体
struct TerrainChunk {
    int logicalX, logicalZ;        // 区块逻辑坐标 (比如 0, 1)
    float worldX, worldZ;          // 转换后的物理起始坐标
    ID3D11Buffer* pVertexBuffer = nullptr;   // 固定的 DX11 显存块
    cudaGraphicsResource_t pCudaRes = nullptr; // 固定的 CUDA 映射句柄
    bool isActive = false;         // 是否正在被使用
};

// 外部 CUDA 函数 (注意参数变成了 worldOffsetX 和 worldOffsetZ)
extern "C" void LaunchTerrainKernel(TerrainVertex* d_vertices, int gridSize, float worldOffsetX, float worldOffsetZ);

class TerrainInteropManager {
private:
    const int GRID_SIZE = 128;            // 每个 Chunk 128x128 个顶点 (太大容易卡顿)
    const float SPACING = 1.5f;           // 顶点间距
    const float CHUNK_SIZE = (GRID_SIZE - 1) * SPACING; // 单个 Chunk 的物理边长 = 190.5 米
    const int VIEW_DISTANCE = 2;          // 渲染半径 (2 代表 5x5 = 25 个区块)
    const int POOL_SIZE = 36;             // 显存池容量 (必须大于渲染区块数)
    int m_verticesPerChunk = 0;

    std::vector<TerrainChunk> m_chunkPool; // 物理显存池
    std::map<std::pair<int, int>, TerrainChunk*> m_activeChunks; // 活跃区块字典

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
    // 1. 初始化系统：一次性申请所有显存
    // ==========================================
    bool Initialize(ID3D11Device* d3dDevice) {
        m_verticesPerChunk = (GRID_SIZE - 1) * (GRID_SIZE - 1) * 6;

        unsigned int deviceCount = 0;
        int cudaDevice = 0;
        cudaError_t err = cudaD3D11GetDevices(&deviceCount, &cudaDevice, 1, d3dDevice, cudaD3D11DeviceListAll);
        if (err != cudaSuccess || deviceCount == 0) return false;
        cudaSetDevice(cudaDevice);

        m_chunkPool.resize(POOL_SIZE);

        for (int i = 0; i < POOL_SIZE; ++i) {
            D3D11_BUFFER_DESC bd = {};
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.ByteWidth = sizeof(TerrainVertex) * m_verticesPerChunk;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

            if (FAILED(d3dDevice->CreateBuffer(&bd, nullptr, &m_chunkPool[i].pVertexBuffer))) return false;

            err = cudaGraphicsD3D11RegisterResource(&m_chunkPool[i].pCudaRes, m_chunkPool[i].pVertexBuffer, cudaGraphicsRegisterFlagsNone);
            if (err != cudaSuccess) return false;

            m_chunkPool[i].isActive = false;
        }
        return true;
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

                        // 呼叫 CUDA 重新填充这块旧显存
                        cudaGraphicsMapResources(1, &chunk->pCudaRes, 0);
                        TerrainVertex* d_vertices = nullptr;
                        size_t numBytes;
                        cudaGraphicsResourceGetMappedPointer((void**)&d_vertices, &numBytes, chunk->pCudaRes);

                        // 传入当前的物理绝对偏移量
                        LaunchTerrainKernel(d_vertices, GRID_SIZE, chunk->worldX, chunk->worldZ);

                        cudaGraphicsUnmapResources(1, &chunk->pCudaRes, 0);
                    }
                }
            }
        }
    }

    // ==========================================
    // 3. 渲染命令提交：把周围的区块全画出来
    // ==========================================
    void Render(ID3D11DeviceContext* d3dContext) {
        UINT stride = sizeof(TerrainVertex);
        UINT offset = 0;
        d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 遍历所有活着的区块，依次绘制
        for (auto const& [coord, chunk] : m_activeChunks) {
            d3dContext->IASetVertexBuffers(0, 1, &chunk->pVertexBuffer, &stride, &offset);
            d3dContext->Draw(m_verticesPerChunk, 0);
        }
    }

    // ==========================================
    // 4. 打扫战场
    // ==========================================
    void Cleanup() {
        for (auto& chunk : m_chunkPool) {
            if (chunk.pCudaRes) {
                cudaGraphicsUnregisterResource(chunk.pCudaRes);
                chunk.pCudaRes = nullptr;
            }
            if (chunk.pVertexBuffer) {
                chunk.pVertexBuffer->Release();
                chunk.pVertexBuffer = nullptr;
            }
        }
        m_activeChunks.clear();
    }
};