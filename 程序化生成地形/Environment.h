#pragma once
#include <DirectXMath.h>

using namespace DirectX;

class EnvironmentSystem {
public:
    float sunAngle = 0.0f;
    float TotalTime = 0.0f;

    XMMATRIX sunMatrix;
    XMFLOAT4 lightDir;

    XMFLOAT4 WeatherParams = { 0.4f, 0.0f, 0.0f, 0.0f }; // x = 湿度
    XMFLOAT4 SkyParams = { 0.003f, 0.0025f, 0.0f, 0.0f }; // x=米氏, y=瑞利

    void Update(float dt, const XMFLOAT3& playerPos) {
        TotalTime += dt;
        sunAngle += dt * 0.5f; // 太阳公转速度

        float sunDistance = 1200.0f;
        float sunX = playerPos.x + cosf(sunAngle) * sunDistance;
        float sunZ = playerPos.z + sinf(sunAngle) * sunDistance;
        float sunY = 600.0f + sinf(sunAngle * 0.5f) * 200.0f;

        XMVECTOR LightPos = XMVectorSet(sunX, sunY, sunZ, 0.0f);
        XMVECTOR LightTarget = XMVectorSet(playerPos.x, 0.0f, playerPos.z, 0.0f);
        XMMATRIX lightView = XMMatrixLookAtLH(LightPos, LightTarget, XMVectorSet(0, 1, 0, 0));
        XMMATRIX lightProj = XMMatrixOrthographicLH(1200.0f, 1200.0f, 1.0f, 2000.0f);

        sunMatrix = lightView * lightProj;

        float dirX = sunX - playerPos.x;
        float dirY = sunY - 0.0f;
        float dirZ = sunZ - playerPos.z;
        float len = sqrtf(dirX * dirX + dirY * dirY + dirZ * dirZ);
        lightDir = XMFLOAT4(dirX / len, dirY / len, dirZ / len, 0.0f);
    }
};