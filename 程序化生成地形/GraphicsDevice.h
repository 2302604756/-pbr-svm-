#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <functional>
#include <cstdio>
#include <Windows.h>

using Microsoft::WRL::ComPtr;

// 错误检查宏：失败时输出调试信息 + 消息框 + 触发断点
#define DX12_CHECK(expr) \
    do { HRESULT _hr = (expr); if (FAILED(_hr)) { \
        char _msg[512]; sprintf_s(_msg, "DX12 error 0x%08X at %s:%d\n%s", (unsigned)_hr, __FILE__, __LINE__, #expr); \
        OutputDebugStringA(_msg); MessageBoxA(nullptr, _msg, "DX12 Fatal Error", MB_ICONERROR); __debugbreak(); \
    } } while (0)

// =============================================
// D3D12 设备封装：设备 / 命令队列 / 交换链 / fence / 描述符堆
// 多帧缓冲 (in-flight)：NUM_FRAMES 个 CPU 帧槽位，每帧等上一轮该槽位完成
// =============================================
class GraphicsDevice {
public:
    static const UINT NUM_FRAMES = 2;        // CPU 侧多帧缓冲数 (in-flight 帧数)
    static const UINT NUM_BACK_BUFFERS = 2;  // 交换链后台缓冲数

    int m_width = 800, m_height = 600;
    LUID adapterLuid = {};   // 所选 DXGI 适配器的 LUID（CUDA 设备匹配用）

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceCounter = 0;
    UINT64 fenceValues[NUM_FRAMES] = {};     // 每帧槽位对应的 Signal 值
    UINT frameIndex = 0;                     // 当前 CPU 帧槽位
    UINT backBufferIndex = 0;                // 当前交换链后台缓冲索引

    // 描述符堆
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;    // SHADER_VISIBLE
    UINT rtvDescriptorSize = 0;
    UINT dsvDescriptorSize = 0;
    UINT srvDescriptorSize = 0;

    // 交换链后台缓冲 + RTV（偏移 0..NUM_BACK_BUFFERS-1）
    ComPtr<ID3D12Resource> backBuffers[NUM_BACK_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV[NUM_BACK_BUFFERS] = {};

    // 主深度缓冲（DSV 堆偏移 0）
    ComPtr<ID3D12Resource> depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE depthDSV = {};

    // 每帧命令分配器 + 常驻命令列表
    ComPtr<ID3D12CommandAllocator> cmdAllocators[NUM_FRAMES];
    ComPtr<ID3D12GraphicsCommandList> cmdList;

    // 临时命令列表（初始化上传 / CUDA 互操作预同步 barrier 专用）
    ComPtr<ID3D12CommandAllocator> barrierAllocator;
    ComPtr<ID3D12GraphicsCommandList> barrierCmdList;

    ~GraphicsDevice() { if (fenceEvent) CloseHandle(fenceEvent); }

    bool Initialize(HWND hWnd, int width, int height) {
        m_width = width; m_height = height;

#ifdef _DEBUG
        // 调试层必须在创建设备前启用
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
#endif

        // 1. 创建设备：枚举硬件适配器，优先 NVIDIA（CUDA 互操作要求 D3D12 与 CUDA 在同一 GPU）
        ComPtr<IDXGIFactory4> factory;
        DX12_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        {
            ComPtr<IDXGIAdapter1> chosen;
            DXGI_ADAPTER_DESC1 chosenDesc = {};
            for (UINT i = 0; ; ++i) {
                ComPtr<IDXGIAdapter1> a;
                if (FAILED(factory->EnumAdapters1(i, &a))) break;
                DXGI_ADAPTER_DESC1 d = {};
                a->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;   // 跳过 WARP/软渲染
                chosen = a; chosenDesc = d;
                if (d.VendorId == 0x10DE) break;                       // NVIDIA 优先
            }
            if (!chosen) return false;
            adapterLuid = chosenDesc.AdapterLuid;
            DX12_CHECK(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        }

        // 2. 创建命令队列
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        DX12_CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

        // 3. 使用已创建的工厂 + Flip 模型交换链

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = width; scd.Height = height;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.Stereo = FALSE;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = NUM_BACK_BUFFERS;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        scd.Flags = 0;

        ComPtr<IDXGISwapChain1> sc1;
        DX12_CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hWnd, &scd, nullptr, nullptr, &sc1));
        DX12_CHECK(sc1.As(&swapChain));
        DX12_CHECK(factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
        backBufferIndex = swapChain->GetCurrentBackBufferIndex();

        // 4. 描述符堆（容量按需求预留）
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 8;   // 2 backbuffer + 4 RG RTV
        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 4;   // 1 主深度 + 2 RG 深度
        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap)));
        dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 16;  // 7 纹理 + 4 RG SRV + 余量
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DX12_CHECK(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap)));
        srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // 5. backbuffer RTV（描述符偏移 0..1）
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i) {
            DX12_CHECK(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
            device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtvHandle);
            backBufferRTV[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }

        // 6. 主深度缓冲（R32_TYPELESS + D32_FLOAT DSV，DSV 堆偏移 0）
        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = width; depthDesc.Height = height;
        depthDesc.DepthOrArraySize = 1; depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        DX12_CHECK(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&depthBuffer)));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc = {};
        dsvViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        depthDSV = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(depthBuffer.Get(), &dsvViewDesc, depthDSV);

        // 7. fence + 事件
        DX12_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // 8. 命令分配器 + 常驻命令列表
        for (UINT i = 0; i < NUM_FRAMES; ++i) {
            DX12_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocators[i])));
        }
        DX12_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocators[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)));
        cmdList->Close();

        // 临时命令列表（barrier / 上传用）
        DX12_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&barrierAllocator)));
        DX12_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, barrierAllocator.Get(), nullptr, IID_PPV_ARGS(&barrierCmdList)));
        barrierCmdList->Close();

        return true;
    }

    // 等待指定帧槽位完成（带超时诊断，便于定位 GPU 挂起/设备移除）
    void WaitForFrame(UINT idx) {
        if (fence->GetCompletedValue() < fenceValues[idx]) {
            DX12_CHECK(fence->SetEventOnCompletion(fenceValues[idx], fenceEvent));
            if (WaitForSingleObject(fenceEvent, 5000) == WAIT_TIMEOUT) {
                HRESULT removed = device->GetDeviceRemovedReason();
                // 收集调试层消息
                char detail[4096] = {};
                ComPtr<ID3D12InfoQueue> iq;
                if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&iq)))) {
                    UINT64 num = iq->GetNumStoredMessages();
                    for (UINT64 i = 0; i < num && i < 15; ++i) {
                        SIZE_T len = 0;
                        iq->GetMessage(i, nullptr, &len);
                        if (!len) continue;
                        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                        if (m) {
                            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription) {
                                strncat_s(detail, m->pDescription, _TRUNCATE);
                                strncat_s(detail, "\n", _TRUNCATE);
                            }
                            free(m);
                        }
                    }
                }
                char title[256];
                sprintf_s(title, "GPU TIMEOUT rem=0x%08X", (unsigned)removed);
                MessageBoxA(nullptr, detail[0] ? detail : "no debug messages", title, MB_ICONERROR);
                __debugbreak();
            }
        }
    }

    // 每帧开始：等上一轮该槽位的帧完成，然后重置分配器与命令列表
    void BeginFrame() {
        WaitForFrame(frameIndex);
        DX12_CHECK(cmdAllocators[frameIndex]->Reset());
        DX12_CHECK(cmdList->Reset(cmdAllocators[frameIndex].Get(), nullptr));
        backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    }

    // 提交一个临时命令列表并等待完成（初始化上传 / CUDA 预同步 barrier）
    void ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        DX12_CHECK(barrierAllocator->Reset());
        DX12_CHECK(barrierCmdList->Reset(barrierAllocator.Get(), nullptr));
        record(barrierCmdList.Get());
        DX12_CHECK(barrierCmdList->Close());
        ID3D12CommandList* lists[] = { barrierCmdList.Get() };
        queue->ExecuteCommandLists(1, lists);
        fenceValues[frameIndex] = ++fenceCounter;
        DX12_CHECK(queue->Signal(fence.Get(), fenceValues[frameIndex]));
        WaitForFrame(frameIndex);
    }

    // 便捷包装：提交单个状态转换并等待完成
    void SubmitBarrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        ExecuteImmediate([&](ID3D12GraphicsCommandList* cl) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
        });
    }

    // 每帧结束：提交命令、发信号、Present，推进帧槽位
    void EndFrame(bool vsync = false) {
        DX12_CHECK(cmdList->Close());
        ID3D12CommandList* lists[] = { cmdList.Get() };
        queue->ExecuteCommandLists(1, lists);
        fenceValues[frameIndex] = ++fenceCounter;
        DX12_CHECK(queue->Signal(fence.Get(), fenceValues[frameIndex]));
        swapChain->Present(vsync ? 1 : 0, 0);
        frameIndex = (frameIndex + 1) % NUM_FRAMES;
        backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    }

    // 等待 GPU 完全空闲（退出时用）
    void WaitForGPU() {
        fenceValues[frameIndex] = ++fenceCounter;
        DX12_CHECK(queue->Signal(fence.Get(), fenceValues[frameIndex]));
        WaitForFrame(frameIndex);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE SRVHeapStartGPU() const { return srvHeap->GetGPUDescriptorHandleForHeapStart(); }
};