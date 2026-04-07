// ==========================================
// 1. 绝对安全的 C++ 通信包裹
// ==========================================
cbuffer ConstantBuffer : register(b0)
{
    matrix WorldViewProj;
    matrix LightViewProj;
    matrix InvViewProj;
    float4 LightDirToSun;
    float4 CameraPos;
    float4 WeatherParams;
    float Time;
    float3 Padding;
    float4 SkyParams; // x=米氏系数(Mie), y=瑞利系数(Rayleigh)
}

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// ==========================================
// 2. 全屏三角形 (深度锁定 Z=W)
// ==========================================
PS_INPUT VS(uint vertexID : SV_VertexID)
{
    PS_INPUT output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Pos = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.Pos.z = output.Pos.w; // 锁定在最远天际线
    return output;
}

// ==========================================
// 3. 防弹版大气散射数学
// ==========================================
static const float PI = 3.14159265359f;

float MiePhase(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (PI * pow(1.0f + g2 - 2.0f * g * cosTheta, 1.5f));
}

float4 PS(PS_INPUT input) : SV_Target
{
    // 1. 重建极其精确的视线方向 (V)
    float2 ndcCoords = input.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 worldPos = mul(float4(ndcCoords, 0.0f, 1.0f), InvViewProj);
    worldPos.xyz /= worldPos.w;
    
    float3 V = normalize(worldPos.xyz - CameraPos.xyz);
    float3 L = normalize(LightDirToSun.xyz);

    // 2. 物理光影混合
    float cosTheta = dot(V, L);
    
    // 提取系数 (加入极小值兜底，防止变为 0)
    float mieCoeff = max(SkyParams.x, 0.0001f);
    float rayleighCoeff = max(SkyParams.y, 0.0001f);

    // 瑞利散射 (基础天空蓝)
    float rayleighPhase = 0.75f * (1.0f + cosTheta * cosTheta);
    float3 rayleigh = float3(0.3f, 0.5f, 0.8f) * rayleighPhase * rayleighCoeff * 500.0f;

    // 米氏散射 (太阳光晕白)
    float miePhase = MiePhase(cosTheta, 0.9f);
    float3 mie = float3(1.0f, 0.9f, 0.8f) * miePhase * mieCoeff * 100.0f;
    
    // 【终极兜底】：哪怕散射全灭，也有一层深蓝色的底漆，绝不会黑屏！
    float3 baseSky = float3(0.05f, 0.1f, 0.2f) * saturate(V.y + 0.2f);
    
    float3 skyColor = baseSky + rayleigh + mie;

    // 3. 色调映射 (Tone Mapping)
    float exposure = 1.5f;
    skyColor = skyColor * exposure;
    skyColor = skyColor / (skyColor + float3(1.0f, 1.0f, 1.0f));
    skyColor = pow(skyColor, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));

    return float4(skyColor, 1.0f);
}