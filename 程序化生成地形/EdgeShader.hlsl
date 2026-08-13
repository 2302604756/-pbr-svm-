// 全屏后处理的结构体
struct VS_OUTPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

Texture2D SceneTexture : register(t0);
SamplerState WrapSampler : register(s0);

// 我们复用你引擎里的 BlurParams 结构体传屏幕参数
cbuffer PostProcessParams : register(b1)
{
    float TexelWidth; // 1.0 / 800.0
    float TexelHeight; // 1.0 / 600.0
    float EdgeThreshold; // 边缘阈值 (比如 0.2)
    float Pad;
};

// 提取颜色的亮度 (Luminance)
float GetLuminance(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

float4 EdgePS(VS_OUTPUT input) : SV_TARGET
{
    float2 uv = input.Tex;
    float w = TexelWidth;
    float h = TexelHeight;

    // 1. 采样周围 9 个点的亮度
    float top_left = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(-w, -h)).rgb);
    float top = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(0, -h)).rgb);
    float top_right = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(w, -h)).rgb);
    
    float mid_left = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(-w, 0)).rgb);
    float center = GetLuminance(SceneTexture.Sample(WrapSampler, uv).rgb); // 原图中心点
    float mid_right = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(w, 0)).rgb);
    
    float bot_left = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(-w, h)).rgb);
    float bot = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(0, h)).rgb);
    float bot_right = GetLuminance(SceneTexture.Sample(WrapSampler, uv + float2(w, h)).rgb);

    // 2. Sobel 算子求 X 和 Y 方向的梯度
    float Gx = -top_left - 2.0 * mid_left - bot_left + top_right + 2.0 * mid_right + bot_right;
    float Gy = -top_left - 2.0 * top - top_right + bot_left + 2.0 * bot + bot_right;

    // 3. 计算综合边缘强度
    float edge = sqrt(Gx * Gx + Gy * Gy);

    // 4. 判断并输出：超过阈值的画黑色描边，否则保留原画面
    float3 originalColor = SceneTexture.Sample(WrapSampler, uv).rgb;
    
    // 如果 edge 大于阈值，说明是边缘，我们把它涂成纯黑。
    // 使用 smoothstep 让边缘过渡稍微平滑一点，不至于全是满屏狗牙
    float edgeFactor = smoothstep(EdgeThreshold - 0.05, EdgeThreshold + 0.05, edge);
    
    float3 finalColor = lerp(originalColor, float3(0, 0, 0), edgeFactor);

    return float4(finalColor, 1.0f);
}