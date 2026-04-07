#pragma once
#pragma once

#include <d3d11.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <iostream>

// 保持与 CUDA 代码中的结构体完全一致
struct TerrainVertex {
    float x, y, z;       // 12 字节
    float nx, ny, nz;    // 12 字节
    float u, v;          // 8 字节
};
// 确保 sizeof(TerrainVertex) 等于 32！

// 声明定义在 .cu 文件中的外部函数
extern "C" void LaunchTerrainKernel(TerrainVertex* d_vertices, int gridSize, float offsetZ);

class TerrainInteropManager {
private:
    ID3D11Buffer* m_vertexBuffer = nullptr;
    cudaGraphicsResource_t m_cudaResource = nullptr;

    // 我们生成一个 128 x 128 的大网格
    const int GRID_SIZE = 256;
    int m_totalVertices = 0;
    float m_timeOffset = 0.0f;

public:
    TerrainInteropManager() = default;
    ~TerrainInteropManager() { Cleanup(); }

    // 1. 初始化系统
    bool Initialize(ID3D11Device* d3dDevice) {
        // (N-1) * (N-1) 个格子，每个格子 2 个三角形 (6个点)
        m_totalVertices = (GRID_SIZE - 1) * (GRID_SIZE - 1) * 6;

        // 让 CUDA 绑定到当前运行 DX11 的这块显卡上
        unsigned int deviceCount = 0;
        int cudaDevice = 0;
        // 注意这里用的是 GetDevices (复数形式)，它完美兼容 ID3D11Device*
        cudaError_t err = cudaD3D11GetDevices(&deviceCount, &cudaDevice, 1, d3dDevice, cudaD3D11DeviceListAll);

        if (err != cudaSuccess || deviceCount == 0) {
            std::cerr << "CUDA 获取 DX11 设备失败!" << std::endl;
            return false;
        }
        cudaSetDevice(cudaDevice);

        // 创建 DX11 顶点缓冲区 (只分配显存，不填数据，数据由 CUDA 算)
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(TerrainVertex) * m_totalVertices;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        HRESULT hr = d3dDevice->CreateBuffer(&bd, nullptr, &m_vertexBuffer);
        if (FAILED(hr)) {
            std::cerr << "DX11 创建顶点缓冲区失败!" << std::endl;
            return false;
        }

        // 核心：把这个 DX11 资源注册进 CUDA 系统，办好“暂住证”
        err = cudaGraphicsD3D11RegisterResource(&m_cudaResource, m_vertexBuffer, cudaGraphicsRegisterFlagsNone);
        if (err != cudaSuccess) {
            std::cerr << "CUDA 注册 DX11 资源失败!" << std::endl;
            return false;
        }

        return true;
    }

    // 2. 更新系统 (每帧调用)
    void Update(float deltaTime) {
        // 让地形随着时间不断向 Z 轴前方延伸 (产生向前飞行的错觉)
        m_timeOffset += deltaTime * 20.0f;

        // 拿过画笔：锁定资源，DX11 暂时不能碰
        cudaGraphicsMapResources(1, &m_cudaResource, 0);

        // 获取显存中的指针
        TerrainVertex* d_vertices = nullptr;
        size_t numBytes;
        cudaGraphicsResourceGetMappedPointer((void**)&d_vertices, &numBytes, m_cudaResource);

        // 发射核函数：几万个 CUDA 线程瞬间算出地形高度和颜色
        LaunchTerrainKernel(d_vertices, GRID_SIZE, m_timeOffset);

        // 归还画笔：算完了，解锁资源给 DX11 去画
        cudaGraphicsUnmapResources(1, &m_cudaResource, 0);
    }

    // 3. 渲染命令提交 (每帧调用)
    void Render(ID3D11DeviceContext* d3dContext) {
        if (!m_vertexBuffer) return;

        UINT stride = sizeof(TerrainVertex);
        UINT offset = 0;

        // 设置我们刚刚被 CUDA 填满心血的 Buffer
        d3dContext->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
        d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 让显卡画出来！
        d3dContext->Draw(m_totalVertices, 0);
    }

    // 4. 打扫战场
    void Cleanup() {
        if (m_cudaResource) {
            cudaGraphicsUnregisterResource(m_cudaResource);
            m_cudaResource = nullptr;
        }
        if (m_vertexBuffer) {
            m_vertexBuffer->Release();
            m_vertexBuffer = nullptr;
        }
    }
};