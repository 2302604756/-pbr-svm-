#include <windows.h>
#include <chrono>S
#include "GraphicsDevice.h"
#include "Camera.h"
#include "Environment.h"
#include "RenderGraph.h"
#include "RenderPipeline.h"
#include "TerrainInteropManager.h"

#pragma comment(lib, "d3d11.lib")
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "ModernDX11Engine", NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowW(L"ModernDX11Engine", L"Render Graph 驱动系统", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hWnd, SW_SHOWDEFAULT);

    //各组件初始化
    if (!g_Graphics.Initialize(hWnd, 800, 600)) return -1;
    if (!g_Pipeline.Initialize(g_Graphics)) return -1;
    if (!g_Terrain.Initialize(g_Graphics.pd3dDevice.Get())) return -1;


    


    // 一次性编译整张渲染图！
    g_Pipeline.BuildGraph(g_RenderGraph, g_Graphics, g_Terrain, g_Camera, g_Environment);
    g_RenderGraph.Compile(g_Graphics);

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
            //g_Terrain.UpdateCloudVolume(g_Pipeline.cudaCloudVolumeRes);
            // 1. 逻辑更新
            g_Camera.Update(dt);
            g_Environment.Update(dt, g_Camera.camPos);
            g_Terrain.Update(g_Camera.camPos.x, g_Camera.camPos.z);


            // 3. 【一键起飞】：根据有向无环图的拓扑排序，自动发射所有 Pass！
            g_RenderGraph.Execute(g_Graphics);

            g_Graphics.Present();
        }
    }

    g_Terrain.Cleanup();
    return 0;
}