// ==========================================
// 1. C++ 通信包裹 (常量缓冲区)
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
    float4 SkyParams;
}

// ==========================================
// 2. 纹理与采样器资源
// ==========================================
Texture2D ShadowMap : register(t0);
SamplerState Sampler : register(s0);

// 地形材质库 (草地、岩石、雪地)
Texture2D RockAlbedoMap : register(t1);
Texture2D RockNormalMap : register(t2);
Texture2D GrassAlbedoMap : register(t3);
Texture2D GrassNormalMap : register(t4);
Texture2D SnowAlbedoMap : register(t5);
Texture2D SnowNormalMap : register(t6);

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
    // 【核心新增：偷渡原始数据】
    float OriginalY : TEXCOORD2; // 记住被拉平前的真实海拔
    float3 OriginalNormal : TEXCOORD3; // 记住被拉平前的真实山体法线
};

// ------------------------------------------
// 4. 顶点着色器 (VS)
// ------------------------------------------
PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    
    // 【核心新增：在拉平前，先录入档案】
    output.OriginalY = input.Pos.y;
    output.OriginalNormal = input.Normal;
    
    // 【魔法：大禹治水】
    float waterLevel = 25.0f; // 从 12.0f 改成 25.0f，或者你觉得好看的高度
    float3 finalPos = input.Pos;

    if (finalPos.y <= waterLevel + 0.05f)
    {
        finalPos.y = waterLevel;
        output.Normal = float3(0.0f, 1.0f, 0.0f);
    }
    else
    {
        output.Normal = input.Normal;
    }

    output.Pos = mul(float4(finalPos, 1.0f), WorldViewProj);
    output.WorldPos = finalPos;
    output.LightSpacePos = mul(float4(finalPos, 1.0f), LightViewProj);
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
// 7. 玩家主像素着色器 (海陆空 + 动态天气)
// ==========================================
float4 PS(PS_INPUT input) : SV_Target
{
   // ==========================================
    // 1. 提取水面遮罩与净化法线 (同时抬高水位！)
    // ==========================================
    float height = input.WorldPos.y;
    
    // 【核心修复 1：抬高水位】
    // 新的 CUDA 地形更雄伟，谷底海拔变高了，12.0f 的水已经被埋在地底下了！
    // 我们把水位抬高到 25.0f，让水重新淹没盆地！
    float waterLevel = 25.0f;
    
    float waterMask = height <= (waterLevel + 0.02f) ? 1.0f : 1.0f - smoothstep(waterLevel + 0.02f, waterLevel + 0.5f, height);

    float3 vertexNormal = normalize(input.Normal);
    vertexNormal = normalize(lerp(vertexNormal, float3(0.0f, 1.0f, 0.0f), waterMask));

    // 【核心修复 2：拯救崩溃的 TBN 矩阵！】
    float3 T = normalize(float3(1.0f, 0.0f, 0.0f) - vertexNormal * vertexNormal.x);
    // 注意这里！必须是 normalize(cross(T, N))
    float3 B = normalize(cross(T, vertexNormal));
    float3x3 TBN = float3x3(T, B, vertexNormal);

    // ==========================================
    // 2. 陆地生态 (程序化纹理混合 Splatting)
    // ==========================================
    float2 rockUV = input.UV * 2.0f;
    float2 grassUV = input.UV * 4.0f;
    float2 snowUV = input.UV * 3.0f;

    float3 rockAlbedo = RockAlbedoMap.Sample(WrapSampler, rockUV).rgb;
    float3 rockNormal = RockNormalMap.Sample(WrapSampler, rockUV).xyz * 2.0f - 1.0f;
    
    float3 grassAlbedo = GrassAlbedoMap.Sample(WrapSampler, grassUV).rgb;
    float3 grassNormal = GrassNormalMap.Sample(WrapSampler, grassUV).xyz * 2.0f - 1.0f;
    
    float3 snowAlbedo = SnowAlbedoMap.Sample(WrapSampler, snowUV).rgb;
    float3 snowNormal = SnowNormalMap.Sample(WrapSampler, snowUV).xyz * 2.0f - 1.0f;

    // 【核心修复 3：镇压法线贴图的噪点】
    rockNormal.xy *= 0.3f;
    rockNormal = normalize(rockNormal);
    grassNormal.xy *= 0.3f;
    grassNormal = normalize(grassNormal);
    snowNormal.xy *= 0.2f;
    snowNormal = normalize(snowNormal);

    // B. 计算生态权重
    float slope = 1.0f - input.OriginalNormal.y;
    
    // 【核心修复 4：推高雪线！】
    // 你的截图全是白色，是因为新山脉随便都有 50 米高，超过了你之前写的 30.0f 雪线。
    // 我们把雪线推高到 60 米以上，让山腰露出岩石和草地！
    float snowLineNoise = sin(input.WorldPos.x * 0.2f) * cos(input.WorldPos.z * 0.2f) * 4.0f;
    float snowWeight = smoothstep(60.0f + snowLineNoise, 75.0f + snowLineNoise, height);
    
    float rockWeight = smoothstep(0.15f, 0.35f, slope);
    float grassWeight = saturate(1.0f - snowWeight - rockWeight);

    float sumWeight = grassWeight + rockWeight + snowWeight;
    grassWeight /= sumWeight;
    rockWeight /= sumWeight;
    snowWeight /= sumWeight;

    float3 finalLandAlbedo = (grassAlbedo * grassWeight) + (rockAlbedo * rockWeight) + (snowAlbedo * snowWeight);
    float3 finalLandNormal = normalize((grassNormal * grassWeight) + (rockNormal * rockWeight) + (snowNormal * snowWeight));
    
    // 【核心修复 5：调高粗糙度，吸收高频闪烁】
    float finalLandRoughness = (0.8f * grassWeight) + (0.85f * rockWeight) + (0.7f * snowWeight);
    float3 finalLandF0 = float3(0.04f, 0.04f, 0.04f);
    
    // D. 动态天气拦截站 (只对陆地生效)
    float globalWetness = WeatherParams.x;

    // 整体受潮变暗变滑
    finalLandAlbedo = lerp(finalLandAlbedo, finalLandAlbedo * 0.35f, globalWetness);
    finalLandRoughness = lerp(finalLandRoughness, 0.15f, globalWetness);

    // 坑洼积水魔法
    float flatFactor = saturate((vertexNormal.y - 0.7f) * 3.3f);
    float puddleNoise = saturate(sin(input.UV.x * 15.0f) * cos(input.UV.y * 15.0f) * 0.5f + 0.5f);
    float puddleMask = flatFactor * puddleNoise * globalWetness;

    float3 terrainRippleNormal;
    terrainRippleNormal.x = (sin(input.WorldPos.x * 5.0f + Time * 3.0f) + sin(input.WorldPos.z * 3.0f - Time * 1.5f)) * 0.03f;
    terrainRippleNormal.y = (cos(input.WorldPos.z * 5.0f + Time * 3.0f) + cos(input.WorldPos.x * 4.0f - Time * 2.0f)) * 0.03f;
    terrainRippleNormal.z = 1.0f;
    terrainRippleNormal = normalize(terrainRippleNormal);

    finalLandNormal = lerp(finalLandNormal, terrainRippleNormal, puddleMask);
    finalLandRoughness = lerp(finalLandRoughness, 0.02f, puddleMask);
    finalLandF0 = lerp(finalLandF0, float3(0.02f, 0.02f, 0.02f), puddleMask);

    // 陡坡雨水冲刷流淌魔法
    float slopeMask = saturate((1.0f - vertexNormal.y) * 2.0f);
    float2 flowingUV = input.UV + float2(0.0f, Time * -0.1f);
    float3 flowingTangentNormal = RockNormalMap.Sample(WrapSampler, flowingUV).xyz * 2.0f - 1.0f;
    finalLandNormal = lerp(finalLandNormal, flowingTangentNormal, slopeMask * globalWetness * 0.5f);

   // ==========================================
// 3. 海洋透射与焦散 (PBR 改进版)
// ==========================================
// 1. 算真实水深
    float trueWaterDepth = max(0.0f, waterLevel - input.OriginalY);

// 2. [关键修正] 定义双介质的吸收系数
// 纯水：吸收红光(0.35)，保留蓝光(0.01) -> 蓝绿色
    float3 waterExtinction = float3(0.35f, 0.05f, 0.01f);
// 泥沙杂质：强烈吸收蓝光(0.4)和绿光(0.1)，保留红光(0.02) -> 黄褐色
    float3 dirtExtinction = float3(0.02f, 0.1f, 0.4f);

    float turbidity = 0.0f; // 浑浊度现在只控制“泥沙”的浓度

// 3. 混合总衰减系数
// 水体永远存在，而泥沙随着浑浊度增加而增加
    float3 totalExtinction = waterExtinction + dirtExtinction * turbidity;

// 4. 计算透射率
    float3 transmittance = exp(-4 *trueWaterDepth * totalExtinction);

// 5. 水底画面透射 (海底颜色 + 焦散)
// 如果水很浑浊，totalExtinction 的 B 通道会很大，蓝光透射率为 0
// 这时光打到水底反射上来，就会呈现浑浊的黄绿色或泥浆色，绝不会变蓝！
    float3 underwaterColor = (finalLandAlbedo + finalLandAlbedo) * transmittance;

// 6. 体积光近似 (In-scattering)
// 这里也需要根据浑浊度混合原本的深水蓝和泥浆色，让散射出的体积光也匹配水质
    float3 deepWaterColor = float3(0.015f, 0.05f, 0.12f);
    float3 muddyWaterColor = float3(0.2f, 0.15f, 0.05f); // 泥浆本色
    float3 currentVolumeColor = lerp(deepWaterColor, muddyWaterColor, saturate(turbidity * 0.2f));

    float3 waterVolumeColor = currentVolumeColor * (1.0f - transmittance);

// 7. 最终合成
    float3 waterAlbedo = underwaterColor + waterVolumeColor;
// 粗糙度与F0保持不变
    float waterRoughness = 0.02f;
    float3 waterF0 = float3(0.02f, 0.02f, 0.02f);

    // 双层法线滚动 (保持你原来的代码)
    float2 waterUV = input.WorldPos.xz * 0.04f;
    float2 scroll1 = waterUV + float2(Time * 0.02f, Time * 0.015f);
    float2 scroll2 = waterUV * 1.5f + float2(-Time * 0.025f, Time * 0.01f);
    float3 normal1 = RockNormalMap.Sample(WrapSampler, scroll1).xyz * 2.0f - 1.0f;
    float3 normal2 = RockNormalMap.Sample(WrapSampler, scroll2).xyz * 2.0f - 1.0f;
    float3 waveNormal = normal1 + normal2;
    waveNormal.xy *= 0.15f;
    waveNormal.z = 1.0f;
    waveNormal = normalize(waveNormal);

    // ==========================================
    // 4. 完美混合：陆地(含生态天气) 与 海洋
    // ==========================================
    float3 albedo = lerp(finalLandAlbedo, waterAlbedo, waterMask);
    float roughness = lerp(finalLandRoughness, waterRoughness, waterMask);
    float3 tangentNormal = lerp(finalLandNormal, waveNormal, waterMask);
    float3 F0 = lerp(finalLandF0, waterF0, waterMask);

    // ==========================================
    // 5. 光照计算 (PBR & VSM)
    // ==========================================
    
    float3 N = normalize(mul(tangentNormal, TBN));
    
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 L = normalize(LightDirToSun.xyz);
    float3 H = normalize(V + L);
    
    float shadowFactor = 1.0f;
    float3 projCoords = input.LightSpacePos.xyz / input.LightSpacePos.w;
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = -projCoords.y * 0.5f + 0.5f;
    
    if (projCoords.x >= 0.0f && projCoords.x <= 1.0f && projCoords.y >= 0.0f && projCoords.y <= 1.0f && projCoords.z >= 0.0f)
    {
        float2 moments = ShadowMap.Sample(Sampler, projCoords.xy).rg;
        float currentDepth = projCoords.z ;
        
        // Depth Bias (深度偏移)：完美过滤自发网格阴影，接收高山阴影
        float bias = 0.0005f;
        
        if (currentDepth > moments.x + bias)
        {
            float variance = max(moments.y - (moments.x * moments.x), 0.00005f);
            float d = currentDepth - moments.x;
            float p_max = variance / (variance + d * d);
            shadowFactor = smoothstep(0.1f, 1.0f, p_max);
        }
    }

    // 水面具有半透明特性，阴影做减弱处理，看起来更清透自然
    shadowFactor = lerp(shadowFactor, 1.0f, waterMask * 0.3f);

    // PBR BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = fresnelSchlick(max(dot(H, V), 0.0f), F0);

    float3 kD = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - 0.0f); // metallic = 0
    float3 specular = (NDF * G * F) / (4.0f * max(dot(N, V), 0.0f) * max(dot(N, L), 0.0f) + 0.0001f);
    
    float3 Lo = (kD * albedo / PI + specular) * float3(8.0f, 7.5f, 7.0f) * max(dot(N, L), 0.0f) * shadowFactor;
    
    float ambientWeight = vertexNormal.y * 0.5f + 0.5f;
    float3 ambient = lerp(float3(0.02f, 0.01f, 0.01f), float3(0.12f, 0.15f, 0.25f), ambientWeight) * albedo;
    
    float3 color = ambient + Lo;

    // ==========================================
    // 6. 注入天空反射与大气雾气
    // ==========================================
    float3 reflectionDir = reflect(-V, N);
    float skyBlend = smoothstep(0.0f, 1.0f, reflectionDir.y);
    float3 fakeSkyReflection = lerp(float3(0.45f, 0.55f, 0.70f), float3(0.10f, 0.20f, 0.40f), skyBlend);
    
    color += fakeSkyReflection * F * waterMask * 2.5f;

    float dist = distance(CameraPos.xyz, input.WorldPos);
    float fogFactor = smoothstep(150.0f, 1000.0f, dist);
    color = lerp(color, float3(0.45f, 0.55f, 0.70f), fogFactor * 0.85f);

    // Tone Mapping
    color = color * 1.5f;
    color = color / (color + float3(1.0f, 1.0f, 1.0f));
    color = pow(color, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));

    color = smoothstep(0.0f, 1.0f, color);
    
    return float4(color, 1.0f);
}

// ==========================================
// 8. VSM 专用高斯模糊着色器
// ==========================================
cbuffer BlurParams : register(b1)
{
    float2 BlurDir;
    float2 BlurPadding;
}

struct BLUR_VS_OUTPUT
{
    float4 Pos : SV_POSITION;
};

float2 BlurPS(BLUR_VS_OUTPUT input) : SV_Target
{
    float2 uv = input.Pos.xy / 2048.0f;

    float2 result = float2(0.0f, 0.0f);
    float weights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };
    
    result += ShadowMap.Sample(Sampler, uv).rg * weights[0];
    
    for (int i = 1; i < 5; ++i)
    {
        float2 offset = BlurDir * (float) i;
        result += ShadowMap.Sample(Sampler, uv + offset).rg * weights[i];
        result += ShadowMap.Sample(Sampler, uv - offset).rg * weights[i];
    }
    
    return result;
}