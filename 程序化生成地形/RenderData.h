#pragma once
#include <DirectXMath.h>

using namespace DirectX;

// 极其关键：强行凑齐 16 字节的倍数
struct ConstantBufferStruct {
    XMMATRIX WorldViewProj;  // 64 bytes
    XMMATRIX LightViewProj;  // 64 bytes
    XMMATRIX InvViewProj;    // 64 bytes
    XMFLOAT4 LightDirToSun;  // 16 bytes
    XMFLOAT4 CameraPos;      // 16 bytes
    XMFLOAT4 WeatherParams;  // 16 bytes (x=湿度)
    float Time;              // 4 bytes
    float Padding[3];        // 12 bytes
    XMFLOAT4 SkyParams;      // 16 bytes (x=米氏, y=瑞利)
};

// 模糊专用的常量缓冲区 (16字节)
struct BlurParams {
    float dirX;
    float dirY;
    float pad1;
    float pad2;
};