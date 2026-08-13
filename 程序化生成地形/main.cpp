#include <windows.h>
#include <chrono>
#include "GraphicsDevice.h"
#include "Camera.h"
#include "Environment.h"
#include "dx12RenderGraph.h"
#include "RenderPipeline.h"
#include "TerrainInteropManager.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

//未来实现App层来统一管理这些类并且实现循环功能
GraphicsDevice        g_Graphics;                                   //用于初始化dx组件以及设置main rsv和dsv
Camera                g_Camera(XMFLOAT3(100.0f, 200.0f, 30.0f));   //
EnvironmentSystem     g_Environment;                                //
RenderPipeline        g_Pipeline;                                   //用于管理管线pass和资源
TerrainInteropManager g_Terrain;                                    //

// 定义全局的 Render Graph
RenderGraph           g_RenderGraph;

// 地形共享缓冲的 D3D12 状态跟踪（CUDA 互操作需要 COMMON(写) ↔ VERTEX(渲染) 转换）
D3D12_RESOURCE_STATES g_terrainState = D3D12_RESOURCE_STATE_COMMON;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "ModernDX12Engine", NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowW(L"ModernDX12Engine", L"DX12 Render Graph 引擎", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hWnd, SW_SHOWDEFAULT);

    //各组件初始化
    if (!g_Graphics.Initialize(hWnd, 800, 600)) return -1;
    // 先匹配 CUDA 设备（云层导入发生在 Pipeline::Initialize，必须与 D3D12 同一 GPU）
    TerrainInteropManager::SetCudaDeviceForD3D12(g_Graphics.adapterLuid);
    if (!g_Pipeline.Initialize(g_Graphics)) return -1;
    if (!g_Terrain.Initialize(g_Graphics.device.Get(), g_Graphics.adapterLuid)) return -1;

    // 一次性编译整张渲染图！
    // 描述符堆偏移：backbuffer RTV 占用 [0..1]、主深度 DSV 占用 [0]、材质 SRV 占用 [0..6]
    g_Pipeline.BuildGraph(g_RenderGraph, g_Graphics, g_Terrain, g_Camera, g_Environment);
    g_RenderGraph.Compile(g_Graphics.device.Get(),
        g_Graphics.rtvHeap.Get(), g_Graphics.dsvHeap.Get(), g_Graphics.srvHeap.Get(),
        g_Graphics.rtvDescriptorSize, g_Graphics.dsvDescriptorSize, g_Graphics.srvDescriptorSize,
        2, 1, 7);

    auto lastTime = std::chrono::high_resolution_clock::now();

    MSG msg = {};
    while (WM_QUIT != msg.message) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        else {
            auto currentTime = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

            // 1. 逻辑更新
            g_Camera.Update(dt);
            g_Environment.Update(dt, g_Camera.camPos);

            // 2. 帧同步 + 开始录制（等上一轮该槽位完成）
            g_Graphics.BeginFrame();

            // 3. CUDA 地形更新（先 barrier 回 COMMON，保证 D3D12 状态语义正确）
            if (g_terrainState != D3D12_RESOURCE_STATE_COMMON) {
                g_Graphics.ExecuteImmediate([&](ID3D12GraphicsCommandList* cl) {
                    g_Terrain.ApplyBarriers(cl, g_terrainState, D3D12_RESOURCE_STATE_COMMON);
                });
                g_terrainState = D3D12_RESOURCE_STATE_COMMON;
            }
            g_Terrain.Update(g_Camera.camPos.x, g_Camera.camPos.z);
            //g_Pipeline.UpdateCloudVolume(); // 云层更新：启用前需为云层纹理加同样的 barrier 同步

            // 4. 填充每帧常量缓冲
            g_Pipeline.UpdateFrameCB(g_Graphics, g_Camera, g_Environment);

            auto* cl = g_Graphics.cmdList.Get();

            // 绑定 shader 可见描述符堆（使用描述符表的必要前提）
            ID3D12DescriptorHeap* heaps[] = { g_Graphics.srvHeap.Get() };
            cl->SetDescriptorHeaps(1, heaps);

            // 地形缓冲 COMMON → VERTEX（本帧渲染使用）
            g_Terrain.ApplyBarriers(cl, g_terrainState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            g_terrainState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

            // 更新 Render Graph 中 backbuffer 的指针与 RTV（交换链每帧轮换）
            g_RenderGraph.UpdateImportedResource(g_Pipeline.m_hBackBuffer,
                g_Graphics.backBuffers[g_Graphics.backBufferIndex].Get(),
                g_Graphics.backBufferRTV[g_Graphics.backBufferIndex]);

            // 5. 【一键起飞】：根据有向无环图的拓扑排序，自动发射所有 Pass！
            g_RenderGraph.Execute(cl);

            // 6. backbuffer 转回 PRESENT 状态，Present 交换
            D3D12_RESOURCE_BARRIER presentBarrier = {};
            presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            presentBarrier.Transition.pResource = g_Graphics.backBuffers[g_Graphics.backBufferIndex].Get();
            presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &presentBarrier);

            g_Graphics.EndFrame();
        }
    }

    g_Graphics.WaitForGPU();
    g_Terrain.Cleanup();
    return 0;
}
