// ==========================================
// 1. C++ 通信包裹 (常量缓冲区)
// ==========================================
cbuffer ConstantBuffer : register(b0)
{
    matrix WorldViewProj;
    matrix LightViewProj;
    matrix InvViewProj; // 【修复：必须加上这个给矩阵占位，哪怕地形不用它！】
    float4 LightDirToSun;
    float4 CameraPos;
    float4 WeatherParams; // x = 全局湿度 (0~1)
    float Time; // 【关键修复】接收 C++ 传来的累积时间
    float3 Padding; // 【修复：极其关键的对齐占位符】
    float4 SkyParams;
}

// ==========================================
// 2. 纹理与采样器资源
// ==========================================
Texture2D ShadowMap : register(t0);
SamplerState Sampler : register(s0);

Texture2D AlbedoMap : register(t1);
Texture2D NormalMap : register(t2);
SamplerState WrapSampler : register(s1);

// ==========================================
// 3. 管线数据结构
// ==========================================
struct VS_INPUT
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD;
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float3 WorldPos : POS3D;
    float4 LightSpacePos : TEXCOORD0;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD1;
};

// ==========================================
// 4. 顶点着色器 (VS)
// ==========================================
PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    output.Pos = mul(float4(input.Pos, 1.0f), WorldViewProj);
    output.WorldPos = input.Pos;
    output.LightSpacePos = mul(float4(input.Pos, 1.0f), LightViewProj);
    
    // 平滑透传法线和 UV
    output.Normal = input.Normal;
    output.UV = input.UV;
    
    return output;
}

// ==========================================
// 5. PBR 核心数学库
// ==========================================
static const float PI = 3.14159265359f;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PI * denom * denom;
    return num / max(denom, 0.0000001f);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0f);
    float k = (r * r) / 8.0f;
    float num = NdotV;
    float denom = NdotV * (1.0f - k) + k;
    return num / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

// ==========================================
// 6. 太阳专用阴影着色器 (PASS 1)
// ==========================================
float2 ShadowPS(PS_INPUT input) : SV_Target
{
    float depth = input.Pos.z;
    return float2(depth, depth * depth);
}

// ==========================================
// 7. 玩家主像素着色器 (PASS 2: 动态水滴与 PBR)
// ==========================================
float4 PS(PS_INPUT input) : SV_Target
{
    // ------------------------------------------
    // A. 切线空间 (TBN) 构建
    // ------------------------------------------
    float3 vertexNormal = normalize(input.Normal);
    float3 T = normalize(float3(1.0f, 0.0f, 0.0f) - vertexNormal * vertexNormal.x);
    float3 B = cross(vertexNormal, T);
    float3x3 TBN = float3x3(T, B, vertexNormal);

    // ------------------------------------------
    // B. 基础岩石贴图采样
    // ------------------------------------------
    float3 albedo = AlbedoMap.Sample(WrapSampler, input.UV).rgb;
    float3 tangentNormal = NormalMap.Sample(WrapSampler, input.UV).xyz * 2.0f - 1.0f;
    
    float roughness = 0.65f;
    float metallic = 0.0f;
    float3 F0 = float3(0.04f, 0.04f, 0.04f);

    // ------------------------------------------
    // C. 动态天气拦截站 (Weather Interception)
    // ------------------------------------------
    float globalWetness = WeatherParams.x;

    // 【1. 整体受潮】变暗、变光滑
    albedo = lerp(albedo, albedo * 0.35f, globalWetness);
    roughness = lerp(roughness, 0.15f, globalWetness);

    // 【2. 积水与涟漪魔法】
    float flatFactor = saturate((vertexNormal.y - 0.7f) * 3.3f);
    float puddleNoise = saturate(sin(input.UV.x * 15.0f) * cos(input.UV.y * 15.0f) * 0.5f + 0.5f);
    float puddleMask = flatFactor * puddleNoise * globalWetness;

    // 算出一个随时间荡漾的微波法线
    float3 rippleNormal;
    rippleNormal.x = (sin(input.WorldPos.x * 5.0f + Time * 3.0f) + sin(input.WorldPos.z * 3.0f - Time * 1.5f)) * 0.03f;
    rippleNormal.y = (cos(input.WorldPos.z * 5.0f + Time * 3.0f) + cos(input.WorldPos.x * 4.0f - Time * 2.0f)) * 0.03f;
    rippleNormal.z = 1.0f;
    rippleNormal = normalize(rippleNormal);

    // 水坑处：用涟漪替换岩石凹凸，粗糙度逼近0(镜面)，F0变成水的0.02
    tangentNormal = lerp(tangentNormal, rippleNormal, puddleMask);
    roughness = lerp(roughness, 0.02f, puddleMask);
    F0 = lerp(F0, float3(0.02f, 0.02f, 0.02f), puddleMask);

    // 【3. 陡坡水珠流淌魔法】
    float slopeMask = saturate((1.0f - vertexNormal.y) * 2.0f);
    // UV 随时间向下滚动
    float2 flowingUV = input.UV + float2(0.0f, Time * -0.1f);
    float3 flowingTangentNormal = NormalMap.Sample(WrapSampler, flowingUV).xyz * 2.0f - 1.0f;
    
    // 在陡坡混入流动的法线，形成雨水冲刷感
    tangentNormal = lerp(tangentNormal, flowingTangentNormal, slopeMask * globalWetness * 0.5f);

    // ------------------------------------------
    // D. 最终法线转换
    // ------------------------------------------
    float3 N = normalize(mul(tangentNormal, TBN));

    // ------------------------------------------
    // E. PBR 光照与 VSM 阴影
    // ------------------------------------------
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 L = normalize(LightDirToSun.xyz);
    float3 H = normalize(V + L);
    
    F0 = lerp(F0, albedo, metallic);

    // VSM 阴影遮挡
    float shadowFactor = 1.0f;
    float3 projCoords = input.LightSpacePos.xyz / input.LightSpacePos.w;
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;
    
    if (projCoords.x >= 0.0f && projCoords.x <= 1.0f && projCoords.y >= 0.0f && projCoords.y <= 1.0f && projCoords.z >= 0.0f)
    {
        float2 moments = ShadowMap.Sample(Sampler, projCoords.xy).rg;
        float currentDepth = projCoords.z;
        if (currentDepth > moments.x)
        {
            float variance = max(moments.y - (moments.x * moments.x), 0.00002f);
            float d = currentDepth - moments.x;
            float p_max = variance / (variance + d * d);
            shadowFactor = smoothstep(0.2f, 1.0f, p_max);
        }
    }

    // Cook-Torrance
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = fresnelSchlick(max(dot(H, V), 0.0f), F0);

    float3 kS = F;
    float3 kD = float3(1.0f, 1.0f, 1.0f) - kS;
    kD *= 1.0f - metallic;

    float3 numerator = NDF * G * F;
    float denominator = 4.0f * max(dot(N, V), 0.0f) * max(dot(N, L), 0.0f) + 0.0001f;
    float3 specular = numerator / denominator;

    float NdotL = max(dot(N, L), 0.0f);
    float3 lightColor = float3(8.0f, 7.5f, 7.0f);
    
    float3 Lo = (kD * albedo / PI + specular) * lightColor * NdotL * shadowFactor;
    float3 ambient = float3(0.20f, 0.25f, 0.35f) * albedo;
    float3 color = ambient + Lo;

    // ------------------------------------------
    // F. Tone Mapping & Gamma
    // ------------------------------------------
    float exposure = 1.5f;
    color = color * exposure;
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    color = pow(color, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));

    return float4(color, 1.0f);
}