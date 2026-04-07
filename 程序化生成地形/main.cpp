#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <vector> // 后面用于创建全屏三角形资源
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

// 假设上一轮的 InteropManager 类已经包含进来
#include "TerrainInteropManager.h"

using namespace DirectX;

// 全局 DX11 变量
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pImmediateContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

ID3D11VertexShader* g_pVertexShader = nullptr;
ID3D11PixelShader* g_pPixelShader = nullptr;
ID3D11PixelShader* g_pShadowPixelShader = nullptr;

ID3D11InputLayout* g_pVertexLayout = nullptr;
ID3D11Buffer* g_pConstantBuffer = nullptr;
ID3D11DepthStencilView* g_pDepthStencilView = nullptr;
// VSM 专用资源：用来存 Z 和 Z^2
ID3D11Texture2D* g_pVSMTexture = nullptr;
ID3D11RenderTargetView* g_pVSMRTV = nullptr;    // 太阳写颜色的画板
ID3D11ShaderResourceView* g_pVSMSRV = nullptr;  // 玩家读概率的眼镜

// --- 新增：阴影贴图所需的全局装备 ---
ID3D11Texture2D* g_pShadowMap = nullptr;
ID3D11DepthStencilView* g_pShadowMapDSV = nullptr;    // 太阳用来写深度的画笔
ID3D11ShaderResourceView* g_pShadowMapSRV = nullptr;    // 玩家用来读阴影的眼镜
ID3D11SamplerState* g_pShadowSampler = nullptr;   // 纹理采样器

// 【新增】贴图和重复采样器
ID3D11ShaderResourceView* g_pAlbedoSRV = nullptr; // 岩石底色图
ID3D11ShaderResourceView* g_pNormalSRV = nullptr; // 水珠/岩石法线图
ID3D11SamplerState* g_pWrapSampler = nullptr;     // 允许贴图无限重复的采样器

// ==========================================
// 【新增】自由摄像机状态
// ==========================================
XMFLOAT3 g_camPos = { 100.0f, 100.0f, -30.0f }; // 初始出生点
float g_camYaw = 0.0f;                          // 左右摇头角
float g_camPitch = 0.2f;                        // 上下点头角 (稍微往下看)

POINT g_LastMousePos;                           // 记录鼠标上一帧的位置
bool g_isRMouseDragging = false;                // 记录右键是否按下

// 2. 【新增专门存大气参数的结构体】
struct AtmosphericParams {
    float MieCoeff;     // 米氏散射系数 (控制太阳光晕)
    float RayleighCoeff;// 瑞利散射系数 (控制天空蓝色度)
    float Padding[2];
};


// 为了同时传摄像机矩阵和太阳矩阵，我们建一个结构体
// 修改 main.cpp 里的结构体
struct ConstantBufferStruct {
    XMMATRIX WorldViewProj;  // 玩家摄像机矩阵
    XMMATRIX LightViewProj;  // 太阳摄像机矩阵
    XMMATRIX InvViewProj;   // 【新增这一行】告诉天空 Shader 怎么把像素坐标还原成视线！
    XMFLOAT4 LightDirToSun;  // 【新增】太阳的照射方向！
    XMFLOAT4 CameraPos; // 【新增】告诉 Shader 玩家的眼睛在哪里！
    XMFLOAT4 WeatherParams; // 【新增】x=全局湿度(0~1)，yzw保留备用
    float Time;
    float Padding[3];        // 12 字节 【极其关键：强行凑齐 16 字节的倍数！】
    XMFLOAT4 SkyParams;      // 【修改】：用最稳妥的 float4 传大气参数 (x=米氏, y=瑞利)
};

// 4. 【新增天空盒专用装备】
ID3D11VertexShader* g_pSkyVertexShader = nullptr; // 天空 VS
ID3D11PixelShader* g_pSkyPixelShader = nullptr;   // 天空 PS
ID3D11DepthStencilState* g_pSkyDepthState = nullptr;// 专门的深度状态 (天空调到最远)

TerrainInteropManager   g_TerrainSystem; // 我们的 CUDA 互操作管家

// 窗口消息处理函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// ==========================================
// DX11 万能贴图加载器
// ==========================================
bool LoadTextureToDX11(const char* filename, ID3D11ShaderResourceView** outSRV) {
    int width, height, channels;
    // 强制要求读取 4 个通道 (RGBA)，因为 DX11 最喜欢 R8G8B8A8_UNORM
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
        MessageBoxA(NULL, "贴图加载失败！请检查文件名和路径。", filename, MB_OK | MB_ICONERROR);
        return false;
    }

    // 1. 描述贴图的物理属性
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // 2. 把内存里的像素数据装填进去
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data;
    initData.SysMemPitch = width * 4; // 一行的字节数

    ID3D11Texture2D* pTexture = nullptr;
    g_pd3dDevice->CreateTexture2D(&desc, &initData, &pTexture);

    // 3. 给 Shader 戴上“眼镜” (创建 SRV)
    g_pd3dDevice->CreateShaderResourceView(pTexture, nullptr, outSRV);

    pTexture->Release();
    stbi_image_free(data); // 释放内存，防止泄漏
    return true;
}


// 初始化 DX11 和 Shader
bool InitDX11(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 800;
    sd.BufferDesc.Height = 600;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    // 创建设备和交换链
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pImmediateContext);

    // 设置渲染目标
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
    pBackBuffer->Release();
    //g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);

    // --- 新增：创建深度缓冲区 (Z-Buffer) ---
    D3D11_TEXTURE2D_DESC descDepth = {};
    descDepth.Width = 800;
    descDepth.Height = 600;
    descDepth.MipLevels = 1;
    descDepth.ArraySize = 1;
    descDepth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // 24位深度，8位模板
    descDepth.SampleDesc.Count = 1;
    descDepth.Usage = D3D11_USAGE_DEFAULT;
    descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* pDepthStencil = nullptr;
    g_pd3dDevice->CreateTexture2D(&descDepth, nullptr, &pDepthStencil);

    D3D11_DEPTH_STENCIL_VIEW_DESC descDSV = {};
    descDSV.Format = descDepth.Format;
    descDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_pd3dDevice->CreateDepthStencilView(pDepthStencil, &descDSV, &g_pDepthStencilView);
    pDepthStencil->Release();

    // 【修改原来的这一行】把刚刚创建的深度视图绑上去！
    // 修改前: g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    // 修改后:
    g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, g_pDepthStencilView);

    // 设置视口 (Viewport)
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f };
    g_pImmediateContext->RSSetViewports(1, &vp);

    // 编译并加载 Shader (带完整错误捕捉)
    ID3DBlob* pVSBlob = nullptr;
    ID3DBlob* pPSBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr;

    // 1. 编译 Vertex Shader
    hr = D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &pVSBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            // 如果是 Shader 代码写错了，弹窗显示第几行报的错
            MessageBoxA(hWnd, (char*)errorBlob->GetBufferPointer(), "VS 编译错误!", MB_OK | MB_ICONERROR);
            errorBlob->Release();
        }
        else {
            // 如果连 errorBlob 都没有，说明根本找不到这个文件！
            MessageBoxA(hWnd, "找不到 Shader.hlsl 文件！\n请确认它和 main.cpp 放在同一个文件夹下！", "文件丢失", MB_OK | MB_ICONERROR);
        }
        return false; // 直接退出，防止后面空指针崩溃
    }

    // 2. 编译 Pixel Shader (玩家用的)
    hr = D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pPSBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            MessageBoxA(hWnd, (char*)errorBlob->GetBufferPointer(), "PS 编译错误!", MB_OK | MB_ICONERROR);
            errorBlob->Release();
        }
        return false;
    }

    // ==========================================
    // 3. 【新增】编译 ShadowPS (太阳用的 VSM 着色器)
    // ==========================================
    ID3DBlob* pShadowPSBlob = nullptr;
    hr = D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "ShadowPS", "ps_5_0", 0, 0, &pShadowPSBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            MessageBoxA(hWnd, (char*)errorBlob->GetBufferPointer(), "ShadowPS 编译错误!", MB_OK | MB_ICONERROR);
            errorBlob->Release();
        }
        return false;
    }

    // 创建三个 Shader 对象
    g_pd3dDevice->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    g_pd3dDevice->CreatePixelShader(pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), nullptr, &g_pPixelShader);
    // 【新增】创建太阳专属的 PS
    g_pd3dDevice->CreatePixelShader(pShadowPSBlob->GetBufferPointer(), pShadowPSBlob->GetBufferSize(), nullptr, &g_pShadowPixelShader);

    // 释放 Blob 内存 (保持好习惯)
    pShadowPSBlob->Release();

    // ==========================================
    // 【新增】编译并创建天空 Shader 
    // ==========================================
    ID3DBlob* pSkyVSBlob = nullptr; // 【修复：补上声明】
    ID3DBlob* pSkyPSBlob = nullptr; // 【修复：补上声明】
    // 1. 编译天空 VS (全屏三角形，不需要 input layout)
    hr = D3DCompileFromFile(L"SkyShader.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &pSkyVSBlob, &errorBlob);
    // ... 错误检查保留 ...
    g_pd3dDevice->CreateVertexShader(pSkyVSBlob->GetBufferPointer(), pSkyVSBlob->GetBufferSize(), nullptr, &g_pSkyVertexShader);

    // 2. 编译天空 PS (大气散射数学库)
    hr = D3DCompileFromFile(L"SkyShader.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &pSkyPSBlob, &errorBlob);
    // ... 错误检查保留 ...
    g_pd3dDevice->CreatePixelShader(pSkyPSBlob->GetBufferPointer(), pSkyPSBlob->GetBufferSize(), nullptr, &g_pSkyPixelShader);

    pSkyVSBlob->Release(); pSkyPSBlob->Release();

    // 3. 【新增】创建专门的深度状态：天空调到最后面，且不写入深度
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // 不写入深度，防止遮挡后面的物体
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;     // 允许深度等于 1.0f 的天空被画出来
    g_pd3dDevice->CreateDepthStencilState(&dsDesc, &g_pSkyDepthState);


    // 告诉 DX11 我们的新顶点结构: Position(0-12), Normal(12-24), TexCoord(24-32)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_pd3dDevice->CreateInputLayout(layout, 3, pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), &g_pVertexLayout);
    g_pImmediateContext->IASetInputLayout(g_pVertexLayout);

    // 【修改常量缓冲区大小】用于传矩阵
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(ConstantBufferStruct); // 以前是 sizeof(XMMATRIX)
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_pd3dDevice->CreateBuffer(&bd, nullptr, &g_pConstantBuffer);

    // --- 新增：锻造 Shadow Map (阴影贴图) ---
    // 1. 创建一张 2048x2048 的巨大隐形画纸
    D3D11_TEXTURE2D_DESC shadowDesc = {};
    shadowDesc.Width = 2048;
    shadowDesc.Height = 2048;
    shadowDesc.MipLevels = 1;
    shadowDesc.ArraySize = 1;
    shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS; // Typeless：现在不定义类型，后面各取所需
    shadowDesc.SampleDesc.Count = 1;
    shadowDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE; // 既能当深度图写，又能当纹理读！
    g_pd3dDevice->CreateTexture2D(&shadowDesc, nullptr, &g_pShadowMap);

    // 2. 给太阳准备写深度的“笔”
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_pd3dDevice->CreateDepthStencilView(g_pShadowMap, &dsvDesc, &g_pShadowMapDSV);

    // 3. 给玩家准备读阴影的“眼镜”
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    g_pd3dDevice->CreateShaderResourceView(g_pShadowMap, &srvDesc, &g_pShadowMapSRV);

    // --- 新增：创建 VSM 用的 R32G32_FLOAT 纹理 ---
    D3D11_TEXTURE2D_DESC vsmDesc = {};
    vsmDesc.Width = 2048;
    vsmDesc.Height = 2048;
    vsmDesc.MipLevels = 1;
    vsmDesc.ArraySize = 1;
    vsmDesc.Format = DXGI_FORMAT_R32G32_FLOAT; // 极其关键：R通道存Z，G通道存Z^2
    vsmDesc.SampleDesc.Count = 1;
    vsmDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_pd3dDevice->CreateTexture2D(&vsmDesc, nullptr, &g_pVSMTexture);

    g_pd3dDevice->CreateRenderTargetView(g_pVSMTexture, nullptr, &g_pVSMRTV);
    g_pd3dDevice->CreateShaderResourceView(g_pVSMTexture, nullptr, &g_pVSMSRV);

    // 4. 创建纹理采样器 (告诉显卡怎么读取这张图)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    g_pd3dDevice->CreateSamplerState(&sampDesc, &g_pShadowSampler);

    // 加载 PBR 贴图
    LoadTextureToDX11("rock_albedo.jpg", &g_pAlbedoSRV);
    LoadTextureToDX11("rock_normal.jpg", &g_pNormalSRV);

    // 创建 Wrap 采样器 (地形贴图必须允许平铺重复 D3D11_TEXTURE_ADDRESS_WRAP)
    D3D11_SAMPLER_DESC wrapDesc = {};
    wrapDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    wrapDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    g_pd3dDevice->CreateSamplerState(&wrapDesc, &g_pWrapSampler);

    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // 1. 创建 Windows 窗口
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "DX11CUDA", NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindowW(L"DX11CUDA", L"CUDA 生成地形实时渲染", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hWnd, SW_SHOWDEFAULT);

    // 2. 初始化 DX11
    InitDX11(hWnd);

    // 3. 【核心】初始化我们上一轮写的 CUDA 地形系统！
    g_TerrainSystem.Initialize(g_pd3dDevice);

// 高度拉升到 100，往后退一点，看向更深处的 Z=150
    XMVECTOR Eye = XMVectorSet(100.0f, 100.0f, -30.0f, 0.0f);
    XMVECTOR At = XMVectorSet(100.0f, 0.0f, 150.0f, 0.0f); // 看向地图中心
    XMVECTOR Up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);     // 上方向
    XMMATRIX view = XMMatrixLookAtLH(Eye, At, Up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 800.0f / 600.0f, 0.01f, 1000.0f);
    XMMATRIX worldViewProj = XMMatrixTranspose(view * proj);
    // 把矩阵传给显卡 Shader
    g_pImmediateContext->UpdateSubresource(g_pConstantBuffer, 0, nullptr, &worldViewProj, 0, 0);

    // 4. 主循环 (游戏循环)
    MSG msg = {};
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (WM_QUIT != msg.message) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        else {
            // 计算两帧之间的时间差 (DeltaTime)
            auto currentTime = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;

            // --- 每一帧的更新与渲染 ---

           // A. 让 CUDA 去算新地形
            g_TerrainSystem.Update(dt);

            // ==========================================
            // 每帧开始：提前算好两套矩阵和动态太阳
            // ==========================================
            // 0. 时间魔法：让太阳动起来！(dt 是 Update(dt) 里的帧间隔时间)
            static float sunAngle = 0.0f;
            sunAngle += dt * 0.5f; // 数值越大，太阳公转越快

            // ==========================================
            // 【核心新增】自由摄像机控制 (WASD + 鼠标右键)
            // ==========================================
            float moveSpeed = 50.0f * dt; // 基础移动速度
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) moveSpeed *= 4.0f; // 按住 Shift 开启“上帝疾跑”

            // 1. 鼠标右键旋转视角
            if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
                POINT currentMousePos;
                GetCursorPos(&currentMousePos);
                if (!g_isRMouseDragging) {
                    g_LastMousePos = currentMousePos;
                    g_isRMouseDragging = true;
                }

                // 计算鼠标滑动的距离 (乘以极其微小的灵敏度)
                float dx = (currentMousePos.x - g_LastMousePos.x) * 0.005f;
                float dy = (currentMousePos.y - g_LastMousePos.y) * 0.005f;

                g_camYaw += dx;
                g_camPitch += dy;

                // 限制俯仰角，防止“把脖子扭断” (万向节死锁)
                g_camPitch = max(-XM_PIDIV2 + 0.01f, min(XM_PIDIV2 - 0.01f, g_camPitch));

                // 把鼠标强行锁回原来的位置，实现无限拖拽！
                SetCursorPos(g_LastMousePos.x, g_LastMousePos.y);
            }
            else {
                g_isRMouseDragging = false;
            }

            // 2. 根据 Yaw 和 Pitch 算出摄像机的真实朝向 (三角函数魔法)
            XMVECTOR camForward = XMVectorSet(sinf(g_camYaw) * cosf(g_camPitch), -sinf(g_camPitch), cosf(g_camYaw) * cosf(g_camPitch), 0.0f);
            XMVECTOR camRight = XMVectorSet(cosf(g_camYaw), 0.0f, -sinf(g_camYaw), 0.0f);
            XMVECTOR camUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); // 世界的正上方

            // 3. 键盘 WASD 处理位移
            XMVECTOR pos = XMLoadFloat3(&g_camPos);
            if (GetAsyncKeyState('W') & 0x8000) pos += camForward * moveSpeed;
            if (GetAsyncKeyState('S') & 0x8000) pos -= camForward * moveSpeed;
            if (GetAsyncKeyState('A') & 0x8000) pos -= camRight * moveSpeed;
            if (GetAsyncKeyState('D') & 0x8000) pos += camRight * moveSpeed;
            if (GetAsyncKeyState('E') & 0x8000) pos += camUp * moveSpeed; // E 键直升机上升
            if (GetAsyncKeyState('Q') & 0x8000) pos -= camUp * moveSpeed; // Q 键直升机下降
            XMStoreFloat3(&g_camPos, pos);

            // 4. 重建全新的 3D 视口矩阵
            XMVECTOR Eye = pos;
            XMVECTOR At = pos + camForward; // 看向摄像机当前所在位置的前方
            XMMATRIX view = XMMatrixLookAtLH(Eye, At, camUp);
            XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 800.0f / 600.0f, 0.01f, 1000.0f);
            XMMATRIX playerMatrix = view * proj;

            // 2. 动态太阳矩阵
            // 我们让太阳以地图中心 (100,100) 为圆心做环绕运动，并且高度发生起伏
            float sunX = 100.0f + cosf(sunAngle) * 150.0f;
            float sunZ = 100.0f + sinf(sunAngle) * 150.0f;
            float sunY = 150.0f + sinf(sunAngle * 0.5f) * 80.0f; // 高度在 70 到 230 之间起伏，阴影会拉长缩短！

            XMVECTOR LightPos = XMVectorSet(sunX, sunY, sunZ, 0.0f);
            XMVECTOR LightTarget = XMVectorSet(100.0f, 0.0f, 100.0f, 0.0f);
            XMMATRIX lightView = XMMatrixLookAtLH(LightPos, LightTarget, XMVectorSet(0, 1, 0, 0));
            XMMATRIX lightProj = XMMatrixOrthographicLH(200.0f, 200.0f, 1.0f, 500.0f);
            XMMATRIX sunMatrix = lightView * lightProj;

            // 3. 【关键新增】算出一个从地形“指向太阳”的向量，传给 Shader！
            float dirX = sunX - 100.0f;
            float dirY = sunY - 0.0f;
            float dirZ = sunZ - 100.0f;
            float len = sqrtf(dirX * dirX + dirY * dirY + dirZ * dirZ);
            XMFLOAT4 lightDir = XMFLOAT4(dirX / len, dirY / len, dirZ / len, 0.0f); // 归一化

            // ==========================================
            // PASS 1：太阳的视界 (生成 VSM)
            // ==========================================
            ID3D11ShaderResourceView* nullSRV = nullptr;
            g_pImmediateContext->PSSetShaderResources(0, 1, &nullSRV);

            // 1. 清理 VSM 的画板 (清理成白色，也就是深度无限远 1.0)
            float clearVSM[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            g_pImmediateContext->ClearRenderTargetView(g_pVSMRTV, clearVSM);
            g_pImmediateContext->ClearDepthStencilView(g_pShadowMapDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

            // 2. 绑定太阳的【颜色画板】和【深度图】
            g_pImmediateContext->OMSetRenderTargets(1, &g_pVSMRTV, g_pShadowMapDSV);

            D3D11_VIEWPORT shadowVP = { 0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f };
            g_pImmediateContext->RSSetViewports(1, &shadowVP);

            ConstantBufferStruct cbShadow;
            cbShadow.WorldViewProj = XMMatrixTranspose(sunMatrix);
            cbShadow.LightViewProj = XMMatrixTranspose(sunMatrix);
            cbShadow.LightDirToSun = lightDir;
            g_pImmediateContext->UpdateSubresource(g_pConstantBuffer, 0, nullptr, &cbShadow, 0, 0);

            // 3. 【极其关键】VSM 必须开启太阳的像素着色器！不能传 nullptr 了！
            g_pImmediateContext->VSSetShader(g_pVertexShader, nullptr, 0);
            g_pImmediateContext->VSSetConstantBuffers(0, 1, &g_pConstantBuffer);
            // 我们要在 Shader.hlsl 里加一个专门给太阳用的 PS：ShadowPS
            g_pImmediateContext->PSSetShader(g_pShadowPixelShader, nullptr, 0);

            g_TerrainSystem.Render(g_pImmediateContext);

            // ... 在 PASS 2 之前 ...
            ConstantBufferStruct cbMain;
            cbMain.WorldViewProj = XMMatrixTranspose(playerMatrix);
            cbMain.LightViewProj = XMMatrixTranspose(sunMatrix);
            cbMain.LightDirToSun = lightDir;
            // 【新增这一行】Eye 就是你在上面定义的 XMVECTOR 摄像机位置
            cbMain.CameraPos = XMFLOAT4(XMVectorGetX(Eye), XMVectorGetY(Eye), XMVectorGetZ(Eye), 1.0f);
            // 【新增这一行】强行注入天气状态！0.0f 是极度干燥，1.0f 是汪洋大海
            cbMain.WeatherParams = XMFLOAT4(0.85f, 0.0f, 0.0f, 0.0f);
            // 【新增这几行】注入时间！dt 是帧间隔，totalTime 是累积时间
            static float totalTime = 0.0f;
            totalTime += dt;
            cbMain.Time = totalTime;

            g_pImmediateContext->UpdateSubresource(g_pConstantBuffer, 0, nullptr, &cbMain, 0, 0);


            // ==========================================
            // PASS 2：玩家的视界 (真正的带光影渲染)
            // ==========================================
            // 1. 切回正常的屏幕和视口
            // 【修复 1：改成接近纯黑的颜色。如果待会跑出来还是黑的，说明天空彻底没画；如果变蓝了，说明天空画成功了！】
            float ClearColor[4] = { 0.05f, 1.0f, 1.0f, 1.0f };
            g_pImmediateContext->ClearRenderTargetView(g_pRenderTargetView, ClearColor);
            g_pImmediateContext->ClearRenderTargetView(g_pRenderTargetView, ClearColor);
            g_pImmediateContext->ClearDepthStencilView(g_pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);
            g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, g_pDepthStencilView);

            D3D11_VIEWPORT mainVP = { 0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f };
            g_pImmediateContext->RSSetViewports(1, &mainVP);

            // 2. 【防踩坑】补上丢失的光栅化状态，防止背面的三角形全消失！
            D3D11_RASTERIZER_DESC rsDesc = {};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_NONE;
            ID3D11RasterizerState* pRasterState;
            g_pd3dDevice->CreateRasterizerState(&rsDesc, &pRasterState);
            g_pImmediateContext->RSSetState(pRasterState);
            pRasterState->Release();

            // 3. 传回真实的矩阵
            //ConstantBufferStruct cbMain;
            cbMain.WorldViewProj = XMMatrixTranspose(playerMatrix);
            cbMain.LightViewProj = XMMatrixTranspose(sunMatrix);
            cbMain.LightDirToSun = lightDir;
            g_pImmediateContext->UpdateSubresource(g_pConstantBuffer, 0, nullptr, &cbMain, 0, 0);

            // 重新挂载玩家的 VS 和主 PS
            g_pImmediateContext->VSSetShader(g_pVertexShader, nullptr, 0);
            g_pImmediateContext->PSSetShader(g_pPixelShader, nullptr, 0);

            // 【把刚画好的 VSM 贴图传进去！】
            g_pImmediateContext->PSSetShaderResources(0, 1, &g_pVSMSRV);
            g_pImmediateContext->PSSetSamplers(0, 1, &g_pShadowSampler);
            g_pImmediateContext->PSSetConstantBuffers(0, 1, &g_pConstantBuffer);

            //岩石法线和基础色贴图
            ID3D11ShaderResourceView* srvs[] = { g_pVSMSRV, g_pAlbedoSRV, g_pNormalSRV }; // t0, t1, t2
            g_pImmediateContext->PSSetShaderResources(0, 3, srvs);
            ID3D11SamplerState* samplers[] = { g_pShadowSampler, g_pWrapSampler }; // s0, s1
            g_pImmediateContext->PSSetSamplers(0, 2, samplers);

            g_TerrainSystem.Render(g_pImmediateContext);

            // F. 提交到显示器
            //g_pSwapChain->Present(0, 0);  必须删去 要不然就没有渲染天空盒了

            // ... PASS 2 的前面部分：清理、绑定地形、绑定 VS 和主 PS，执行 g_TerrainSystem.Render(g_pImmediateContext) ...
            // 地形现在画完了

            // ==========================================
            // 【新增】天空覆盖 Pass 
            // ==========================================
            // 1. 计算 InvViewProj (逆矩阵)
            XMMATRIX viewProj = view * proj;
            XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

            // 2. 填充并更新常量缓冲区
            ConstantBufferStruct cbSky = {};
            cbSky.InvViewProj = XMMatrixTranspose(invViewProj); // 【新增这一行】
            cbSky.LightDirToSun = lightDir;
            cbSky.CameraPos = XMFLOAT4(XMVectorGetX(Eye), XMVectorGetY(Eye), XMVectorGetZ(Eye), 1.0f);

            // 设定物理真实的大气参数：你可以随时改这两个值！
            // 设定物理真实的大气参数 (x = 太阳光晕，y = 天空蓝色)
            cbSky.SkyParams = XMFLOAT4(0.003f, 0.0025f, 0.0f, 0.0f);

            g_pImmediateContext->UpdateSubresource(g_pConstantBuffer, 0, nullptr, &cbSky, 0, 0);

            // 3. 【极其关键】绑定天空专用深度状态
            g_pImmediateContext->OMSetDepthStencilState(g_pSkyDepthState, 0);

            // 4. 绑定天空 VS 和 PS 
            // 注意：天空 VS 是全屏三角形，不需要 Input Layout 和顶点缓冲！
           // 【补丁 1：强行把画笔扳回“三角形列表”模式！】
            g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // 【补丁 2：正确的解绑顶点缓冲的方式】
            ID3D11Buffer* nullBuf = nullptr;
            UINT stride = 0, offset = 0;
            g_pImmediateContext->IASetVertexBuffers(0, 1, &nullBuf, &stride, &offset);

            g_pImmediateContext->IASetInputLayout(nullptr);
            g_pImmediateContext->VSSetShader(g_pSkyVertexShader, nullptr, 0);
            g_pImmediateContext->VSSetConstantBuffers(0, 1, &g_pConstantBuffer);
            g_pImmediateContext->PSSetShader(g_pSkyPixelShader, nullptr, 0);
            g_pImmediateContext->PSSetConstantBuffers(0, 1, &g_pConstantBuffer);

            // 5. 【核爆点】全屏三角形绘制 (Draw 3 个无顶点数据的点)
            g_pImmediateContext->Draw(3, 0);

            // 6. 恢复正常的深度状态和 input layout，为下一帧做准备
            g_pImmediateContext->OMSetDepthStencilState(nullptr, 0); // 默认状态
            g_pImmediateContext->IASetInputLayout(g_pVertexLayout);

            g_pSwapChain->Present(0, 0);
        }
    }

    g_TerrainSystem.Cleanup();
    return 0;
}

