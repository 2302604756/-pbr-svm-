#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <math_functions.h>

// 1. 顶点结构体 (与 DX11 输入布局严格保持 16 + 16 = 32 字节对齐)
// 以前是 Position 和 Color，现在升级为 PBR 标准三件套！
struct TerrainVertex {
    float x, y, z;       // 空间物理坐标 (12 bytes)
    float nx, ny, nz;    // 平滑顶点法线 (12 bytes)
    float u, v;          // 纹理映射 UV 坐标 (8 bytes)
};

// ==========================================
// 基础数学与噪声工具箱 (硬编码实现，无需外部库)
// ==========================================
__device__ float lerp(float a, float b, float t) { return a + t * (b - a); }

__device__ float smoothstep_custom(float edge0, float edge1, float x) {
    float t = fmaxf(0.0f, fminf(1.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

// 1. 专业的整数哈希函数 (彻底解决 sin 崩溃导致的闪烁！)
__device__ float hash2D(int x, int z) {
    // 使用大质数进行位运算打散
    unsigned int hX = (unsigned int)(x * 374761393U);
    unsigned int hZ = (unsigned int)(z * 668265263U);
    unsigned int h = (hX ^ hZ) * 1274126177U;
    h = h ^ (h >> 16);
    h = h * 2246822507U;
    h = h ^ (h >> 13);
    // 将 32 位无符号整数规范化为 0.0 到 1.0 之间的浮点数
    return (float)(h & 0x00ffffff) / 16777216.0f;
}

// 2. 匹配新哈希的平滑噪声
__device__ float noise2D(float x, float z) {
    // 强制转为整数网格坐标
    int ix = (int)floorf(x);
    int iz = (int)floorf(z);
    
    // 获取小数点后的尾数，用于插值
    float fx = x - (float)ix;
    float fz = z - (float)iz;

    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fz * fz * (3.0f - 2.0f * fz);

    // 调用新的整数哈希
    float a = hash2D(ix, iz);
    float b = hash2D(ix + 1, iz);
    float c = hash2D(ix, iz + 1);
    float d = hash2D(ix + 1, iz + 1);

    return lerp(lerp(a, b, u), lerp(c, d, u), v);
}
// ==========================================
// 终极地形方程式：山脉、盆地与领地扭曲
// ==========================================
__device__ float CalculateHeight(float x, float z) {
    // 1. 领地扭曲 (Domain Warping)：让山脉走向如流体般弯曲
    float warpX = noise2D(x * 0.01f, z * 0.01f) * 20.0f;
    float warpZ = noise2D(x * 0.01f + 100.0f, z * 0.01f + 100.0f) * 20.0f;

    float nx = x + warpX;
    float nz = z + warpZ;

    // 2. 生物群系遮罩 (Biome Mask)：区分平坦盆地和高耸山脉
    float macroNoise = noise2D(nx * 0.005f, nz * 0.005f);
    // 0.3以下是平原(0)，0.6以上是高山(1)，中间平滑过渡
    float mountainMask = smoothstep_custom(0.3f, 0.6f, macroNoise);

    // 3. FBM 细节生成 (雕刻山脊线)
    float fbmNoise = 0.0f;
    float amplitude = 1.0f;
    float frequency = 0.05f;

    for (int i = 0; i < 20; i++) {
        // 山脊噪声: 1.0 - abs(noise * 2 - 1) 产生刀劈般的山峰    
        //float n = noise2D(nx * frequency, nz * frequency) * 2.0f - 1.0f;
       // n = 1.0f - fabsf(n);
        //n *= n; // 强化尖锐感

        float n = noise2D(nx * frequency, nz * frequency);

        fbmNoise += n * amplitude;
        amplitude *= 0.4f;
        frequency *= 2.0f;
    }

    // 4. 融合组装
    float groundLevel = 5.0f;         // 盆地基础海拔
    float maxMountainHeight = 60.0f;  // 山脉最高隆起

    return groundLevel + (fbmNoise * mountainMask * maxMountainHeight);
}

// ==========================================
// 中心差分法求平滑法线 (微积分魔法)
// ==========================================
__device__ void CalculateNormal(float x, float z, float& nx, float& ny, float& nz) {
    float eps = 0.1f; // 极小偏移量，决定了法线的采样精度

    // 探查四周的高度
    float hL = CalculateHeight(x - eps, z);
    float hR = CalculateHeight(x + eps, z);
    float hD = CalculateHeight(x, z - eps);
    float hU = CalculateHeight(x, z + eps);

    // 根据高差求出偏导数，并推导出法线向量
    nx = hL - hR;
    ny = 2.0f * eps;
    nz = hD - hU;

    // 向量归一化
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    nx /= len; ny /= len; nz /= len;
}

// 【核心数学】利用中心差分法，求出极其平滑的真实顶点法线
__device__ float3 CalculateNormal(float x, float z) {
    float delta = 0.1f; // 极小的探测步长 (步子越小，微分越准)

    // 探测周围四个点的高度
    float hL = CalculateHeight(x - delta, z); // 左
    float hR = CalculateHeight(x + delta, z); // 右
    float hD = CalculateHeight(x, z - delta); // 下 (后)
    float hU = CalculateHeight(x, z + delta); // 上 (前)

    // 计算法线向量 N = cross(Z轴切线, X轴切线)
    float3 n;
    n.x = hL - hR;
    n.y = 2.0f * delta;
    n.z = hD - hU;

    // 归一化 (Normalize)
    float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    return make_float3(n.x / len, n.y / len, n.z / len);
}


// ==========================================
// CUDA 核心入口：生成 DX11 可用的三角形网格
// ==========================================

//第一个参数：输入顶点数组的指针
//第二个参数：网格大小
//第三个参数：时间偏移量，用于控制错觉的飞行速度
// ... 顶部的法线和高度函数不变 ...

// ==========================================
// CUDA 核心：绝对坐标生成
// ==========================================
__global__ void GenerateTerrainKernel(TerrainVertex* vertices, int gridSize, float worldOffsetX, float worldOffsetZ) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int z = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= gridSize - 1 || z >= gridSize - 1) return;

    int index = (z * (gridSize - 1) + x) * 6;
    float spacing = 1.5f;

    // 【核心】网格局部坐标 + 传入的世界起始点偏移量
    float sampleX = (float)x * spacing + worldOffsetX;
    float sampleZ = (float)z * spacing + worldOffsetZ;

    float y00 = CalculateHeight(sampleX, sampleZ);
    float y10 = CalculateHeight(sampleX + spacing, sampleZ);
    float y01 = CalculateHeight(sampleX, sampleZ + spacing);
    float y11 = CalculateHeight(sampleX + spacing, sampleZ + spacing);

    float3 n00 = CalculateNormal(sampleX, sampleZ);
    float3 n10 = CalculateNormal(sampleX + spacing, sampleZ);
    float3 n01 = CalculateNormal(sampleX, sampleZ + spacing);
    float3 n11 = CalculateNormal(sampleX + spacing, sampleZ + spacing);

    float uvScale = 20.0f;
    float2 uv00 = make_float2(sampleX / uvScale, sampleZ / uvScale);
    float2 uv10 = make_float2((sampleX + spacing) / uvScale, sampleZ / uvScale);
    float2 uv01 = make_float2(sampleX / uvScale, (sampleZ + spacing) / uvScale);
    float2 uv11 = make_float2((sampleX + spacing) / uvScale, (sampleZ + spacing) / uvScale);

    // 绝对物理坐标装填
    auto setVertex = [&](int offset, float vx, float vy, float vz, float3 n, float2 uv) {
        vertices[index + offset].x = vx; vertices[index + offset].y = vy; vertices[index + offset].z = vz;
        vertices[index + offset].nx = n.x; vertices[index + offset].ny = n.y; vertices[index + offset].nz = n.z;
        vertices[index + offset].u = uv.x; vertices[index + offset].v = uv.y;
        };

    setVertex(0, sampleX, y00, sampleZ, n00, uv00);
    setVertex(1, sampleX + spacing, y10, sampleZ, n10, uv10);
    setVertex(2, sampleX, y01, sampleZ + spacing, n01, uv01);

    setVertex(3, sampleX, y01, sampleZ + spacing, n01, uv01);
    setVertex(4, sampleX + spacing, y10, sampleZ, n10, uv10);
    setVertex(5, sampleX + spacing, y11, sampleZ + spacing, n11, uv11);
}

extern "C" void LaunchTerrainKernel(TerrainVertex* d_vertices, int gridSize, float worldOffsetX, float worldOffsetZ) {
    dim3 blockSize(16, 16);
    dim3 gridSizeDim((gridSize + blockSize.x - 1) / blockSize.x, (gridSize + blockSize.y - 1) / blockSize.y);
    GenerateTerrainKernel << <gridSizeDim, blockSize >> > (d_vertices, gridSize, worldOffsetX, worldOffsetZ);
    cudaDeviceSynchronize();
}