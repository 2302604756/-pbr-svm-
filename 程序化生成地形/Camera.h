#pragma once
#include <windows.h>
#include <DirectXMath.h>

using namespace DirectX;

class Camera {
public:
    XMFLOAT3 camPos;
    float camYaw;
    float camPitch;
    POINT LastMousePos;
    bool isRMouseDragging;

    Camera(XMFLOAT3 startPos) : camPos(startPos), camYaw(0.0f), camPitch(0.2f), isRMouseDragging(false) {}

    void Update(float dt) {
        float moveSpeed = 50.0f * dt;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) moveSpeed *= 4.0f;

        // 1. 鼠标右键旋转视角
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
            POINT currentMousePos;
            GetCursorPos(&currentMousePos);
            if (!isRMouseDragging) {
                LastMousePos = currentMousePos;
                isRMouseDragging = true;
            }

            float dx = (currentMousePos.x - LastMousePos.x) * 0.005f;
            float dy = (currentMousePos.y - LastMousePos.y) * 0.005f;

            camYaw += dx;
            camPitch += dy;
            camPitch = max(-XM_PIDIV2 + 0.01f, min(XM_PIDIV2 - 0.01f, camPitch));

            SetCursorPos(LastMousePos.x, LastMousePos.y);
        }
        else {
            isRMouseDragging = false;
        }

        // 2. 根据 Yaw 和 Pitch 算出真实朝向
        XMVECTOR camForward = XMVectorSet(sinf(camYaw) * cosf(camPitch), -sinf(camPitch), cosf(camYaw) * cosf(camPitch), 0.0f);
        XMVECTOR camRight = XMVectorSet(cosf(camYaw), 0.0f, -sinf(camYaw), 0.0f);
        XMVECTOR camUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        // 3. 键盘位移
        XMVECTOR pos = XMLoadFloat3(&camPos);
        if (GetAsyncKeyState('W') & 0x8000) pos += camForward * moveSpeed;
        if (GetAsyncKeyState('S') & 0x8000) pos -= camForward * moveSpeed;
        if (GetAsyncKeyState('A') & 0x8000) pos -= camRight * moveSpeed;
        if (GetAsyncKeyState('D') & 0x8000) pos += camRight * moveSpeed;
        if (GetAsyncKeyState('E') & 0x8000) pos += camUp * moveSpeed;
        if (GetAsyncKeyState('Q') & 0x8000) pos -= camUp * moveSpeed;
        XMStoreFloat3(&camPos, pos);
    }

    XMMATRIX GetViewMatrix() const {
        XMVECTOR Eye = XMLoadFloat3(&camPos);
        XMVECTOR Forward = XMVectorSet(sinf(camYaw) * cosf(camPitch), -sinf(camPitch), cosf(camYaw) * cosf(camPitch), 0.0f);
        XMVECTOR At = Eye + Forward;
        return XMMatrixLookAtLH(Eye, At, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }

    XMMATRIX GetProjMatrix(float aspect) const {
        return XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.01f, 1000.0f);
    }
};