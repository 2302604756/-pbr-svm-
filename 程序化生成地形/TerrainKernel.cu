#include <cuda_runtime.h>
#include <device_launch_parameters.h>


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
// ==========================================
// 1. 梯度哈希函数 (生成随机的 2D 向量方向)
// ==========================================
__device__ float2 hash2D_gradient(int x, int z) {
    // 依然使用你优秀的整数位运算，但我们这次要生成一个“方向(Angle)”
    unsigned int hX = (unsigned int)(x * 374761393U);
    unsigned int hZ = (unsigned int)(z * 668265263U);
    unsigned int h = (hX ^ hZ) * 1274126177U;
    h = h ^ (h >> 16);
    h = h * 2246822507U;
    h = h ^ (h >> 13);

    // 将随机整数映射为 0 到 2π 的弧度
    float angle = ((float)(h & 0x00ffffff) / 16777216.0f) * 6.28318530718f;

    // 返回一个单位圆上的随机方向向量
    return make_float2(cosf(angle), sinf(angle));
}

// ==========================================
// 2. 纯正的 Perlin Noise (梯度噪声)
// ==========================================
__device__ float perlin2D(float x, float z) {
    int ix = (int)floorf(x);
    int iz = (int)floorf(z);

    float fx = x - (float)ix;
    float fz = z - (float)iz;

    // 计算当前点到四个角的距离向量
    float2 d00 = make_float2(fx, fz);
    float2 d10 = make_float2(fx - 1.0f, fz);
    float2 d01 = make_float2(fx, fz - 1.0f);
    float2 d11 = make_float2(fx - 1.0f, fz - 1.0f);

    // 获取四个角的随机梯度向量
    float2 g00 = hash2D_gradient(ix, iz);
    float2 g10 = hash2D_gradient(ix + 1, iz);
    float2 g01 = hash2D_gradient(ix, iz + 1);
    float2 g11 = hash2D_gradient(ix + 1, iz + 1);

    // 计算梯度向量与距离向量的“点乘” (Dot Product)
    float a = g00.x * d00.x + g00.y * d00.y;
    float b = g10.x * d10.x + g10.y * d10.y;
    float c = g01.x * d01.x + g01.y * d01.y;
    float d = g11.x * d11.x + g11.y * d11.y;

    // 【关键】使用 C2 连续的五次多项式平滑插值：6t^5 - 15t^4 + 10t^3
    float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    float v = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    // 插值并映射回 0.0 ~ 1.0 的范围，供后续计算使用
    float res = lerp(lerp(a, b, u), lerp(c, d, u), v);
    return res * 0.5f + 0.5f;
}

// ==========================================
// 终极地形方程式：双群系混合 (Biome Blending)
// ==========================================
__device__ float CalculateHeight(float x, float z) {
    // 1. 宏观生物群系划分 (0.0 = 平原/丘陵, 1.0 = 高耸山脉)
    // 降低频率 (0.0015f)，让各个地形的占地面积更大，形成连绵的群系，而不是碎花布
    float macroNoise = perlin2D(x * 0.0015f, z * 0.0015f);
    
    // 使用 smoothstep 将地形强行划分为两极，中间留有自然的平滑过渡区
    float mountainMask = smoothstep_custom(0.35f, 0.65f, macroNoise);

    // ==========================================
    // 地形 A: 柔和的平原与丘陵 (Rolling Hills)
    // 特点：低频，无折线，像丝绸一样起伏，极度柔和
    // ==========================================
    float plainsHeight = 0.0f;
    float pAmp = 1.0f;
    float pFreq = 0.015f;
    for (int i = 0; i < 4; i++) { // 只有 4 层，剥离所有微观噪点
        // 使用最纯粹的平滑 Perlin，不加任何绝对值翻折
        plainsHeight += perlin2D(x * pFreq, z * pFreq) * pAmp;
        pAmp *= 0.5f;
        pFreq *= 2.0f;
    }
    plainsHeight = plainsHeight * 18.0f; // 丘陵最大高度

    // ==========================================
    // 地形 B: 雄伟的独立山脉 (Alpine Mountains)
    // 特点：保留山脊走向，但降低了尖锐度，山体厚重磅礴
    // ==========================================
    // 引入领地扭曲，让山脉走向如流体、地壳运动般弯曲
    float warpX = perlin2D(x * 0.005f, z * 0.005f) * 50.0f;
    float warpZ = perlin2D(x * 0.005f + 100.0f, z * 0.005f + 100.0f) * 50.0f;
    float mx = x + warpX;
    float mz = z + warpZ;

    float mountainHeight = 0.0f;
    float mAmp = 1.0f;
    float mFreq = 0.02f;
    float weight = 1.0f;

    for (int i = 0; i < 6; i++) { // 6 层足够，再多就变“砂纸”了
        float n = perlin2D(mx * mFreq, mz * mFreq) * 2.0f - 1.0f;
        
        // 经典的山脊翻折
        n = 1.0f - fabsf(n); 
        
        // 【核心柔和化魔法】：之前是 n *= n (极其锐利的刀片山)
        // 现在改为 powf(n, 1.2f)，山脊依然存在，但边缘变得圆润厚实！
        n = powf(n, 0.5F); 

        mountainHeight += n * mAmp * weight;
        
        // 权重控制：山谷处依然平滑，只有山腰以上才崎岖
        weight = fmaxf(0.0f, fminf(1.0f, mountainHeight * 1.5f)); 

        mAmp *= 0.45f;
        mFreq *= 2.0f;
    }
    mountainHeight = mountainHeight * 100.0f; // 巍峨高山，拔地而起

    // ==========================================
    // 最终融合：将平原和山脉拼接在一起
    // ==========================================
    float groundLevel = 2.0f; // 最低基础海拔

    // 神奇的 lerp：在 plains 和 mountains 之间平滑过渡
    float finalHeight = groundLevel + lerp(plainsHeight, mountainHeight, mountainMask);

    return finalHeight;
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
    // 【修改点】：把 0.1f 改成 1.0f 甚至 1.5f
    // 步子大一点，相当于自带了“低通滤波 (Low-pass Filter)”，法线会变得极其丝滑
    float delta = 1.0f;

    // 探测周围四个点的高度
    float hL = CalculateHeight(x - delta, z); // 左
    float hR = CalculateHeight(x + delta, z); // 右
    float hD = CalculateHeight(x, z - delta); // 下 (后)
    float hU = CalculateHeight(x, z + delta); // 上 (前)

    // 计算法线向量 N
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
    GenerateTerrainKernel <<<gridSizeDim, blockSize >>> (d_vertices, gridSize, worldOffsetX, worldOffsetZ);
    cudaDeviceSynchronize();
}


// ==========================================
// 3D 噪声工具箱 (用于生成云层体积数据)
// ==========================================
// 1. 3D 整数哈希
__device__ float hash3D(int x, int y, int z) {
    unsigned int hX = (unsigned int)(x * 374761393U);
    unsigned int hY = (unsigned int)(y * 668265263U);
    unsigned int hZ = (unsigned int)(z * 1274126177U);
    unsigned int h = (hX ^ hY ^ hZ) * 2246822507U;
    h = h ^ (h >> 13);
    return (float)(h & 0x00ffffff) / 16777216.0f;
}

// 2. 3D 平滑噪声 (三线性插值)
__device__ float noise3D(float x, float y, float z) {
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    int iz = (int)floorf(z);

    float fx = x - (float)ix;
    float fy = y - (float)iy;
    float fz = z - (float)iz;

    // 平滑曲线 3x^2 - 2x^3
    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fy * fy * (3.0f - 2.0f * fy);
    float w = fz * fz * (3.0f - 2.0f * fz);

    // 采样立方体 8 个顶点的哈希值
    float a0 = hash3D(ix, iy, iz);
    float b0 = hash3D(ix + 1, iy, iz);
    float c0 = hash3D(ix, iy + 1, iz);
    float d0 = hash3D(ix + 1, iy + 1, iz);
    float a1 = hash3D(ix, iy, iz + 1);
    float b1 = hash3D(ix + 1, iy, iz + 1);
    float c1 = hash3D(ix, iy + 1, iz + 1);
    float d1 = hash3D(ix + 1, iy + 1, iz + 1);

    // X 轴插值
    float k0 = lerp(a0, b0, u);
    float k1 = lerp(c0, d0, u);
    float k2 = lerp(a1, b1, u);
    float k3 = lerp(c1, d1, u);

    // Y 轴插值
    float m0 = lerp(k0, k1, v);
    float m1 = lerp(k2, k3, v);

    // Z 轴插值
    return lerp(m0, m1, w);
}

// 3. 3D 分形布朗运动 (FBM)
__device__ float fbm3D(float x, float y, float z) {
    float v = 0.0f;
    float a = 0.5f;
    float freq = 1.0f;
    // 叠加 4 层 3D 噪声 (层数太多会严重影响生成速度，4层打底足够了)
    for (int i = 0; i < 4; ++i) {
        v += a * noise3D(x * freq, y * freq, z * freq);
        freq *= 2.0f;
        a *= 0.5f;
    }
    return v;
}

// ==========================================
// CUDA 核函数：向 3D 纹理写入数据
// ==========================================
// ==========================================
// CUDA 核函数：向 3D 纹理写入 FBM 云层数据
// ==========================================
__global__ void GenerateCloudVolumeKernel(cudaSurfaceObject_t surface, int volumeSize) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    int z = blockIdx.z * blockDim.z + threadIdx.z;

    if (x >= volumeSize || y >= volumeSize || z >= volumeSize) return;

    // 1. 将 3D 坐标映射到 0.0 ~ 1.0
    float u = (float)x / volumeSize;
    float v = (float)y / volumeSize; // Y 轴代表云层高度
    float w = (float)z / volumeSize;

    // 2. 采样 3D FBM 噪声 (控制云团的大小)
    float noiseScale = 5.0f; // 值越大，云团越细碎；值越小，云团越庞大
    float noiseVal = fbm3D(u * noiseScale, v * noiseScale, w * noiseScale);

    // 3. 高度梯度约束 (让云只飘在中间，上下两端消散)
    // 创造一个中间值为 1.0，两端为 0.0 的抛物线覆盖遮罩
    float heightGradient = 1.0f - fabsf(v - 0.5f) * 2.0f;

    // 4. 结合噪声与高度遮罩
    float density = noiseVal * heightGradient;

    // 5. 密度阈值截断 (剔除低密度的雾气，形成大块结实的云朵)
    density = smoothstep_custom(0.35f, 0.7f, density);

    // 将浮点数映射为 0~255 的像素值
    unsigned char val = (unsigned char)(density * 255.0f);

    // 打包写入 3D 纹理 (DX11 端只需要读取 Red 或者 Alpha 通道即可)
    uchar4 pixelData = make_uchar4(val, val, val, val);
    surf3Dwrite(pixelData, surface, x * sizeof(uchar4), y, z);
}
// C++ 呼叫入口
extern "C" void LaunchCloudGenKernel(cudaSurfaceObject_t surface, int volumeSize) {
    dim3 blockSize(8, 8, 8);
    dim3 gridSize(
        (volumeSize + blockSize.x - 1) / blockSize.x,
        (volumeSize + blockSize.y - 1) / blockSize.y,
        (volumeSize + blockSize.z - 1) / blockSize.z
    );
    GenerateCloudVolumeKernel << <gridSize, blockSize >> > (surface, volumeSize);
    cudaDeviceSynchronize();
}
