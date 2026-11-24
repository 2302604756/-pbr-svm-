# Procedural Terrain Renderer (CUDA + Direct3D 12)

> 基于 **CUDA + Direct3D 12** 的程序化地形实时渲染项目，实现异构地形生成、PBR 多地表材质混合、简化水体吸收/透射与方差阴影贴图（VSM）。
>
> 👉 更多近期渲染器与实时全局光照（DDGI / DXR / RenderGraph）工作见主仓库：[Model_Renderer](https://github.com/2302604756/Model_Renderer)。

## 核心特性

- **CUDA / D3D12 异构互操作**：在 CUDA Kernel（`TerrainKernel.cu`）中通过分层 Perlin 噪声生成地形高度与平滑法线，经 `TerrainInteropManager` 共享缓冲直接提交给 D3D12 管线渲染。
- **基于高度与坡度的多地表混合**：依据地形高度与法线坡度权重平滑混合草地、岩石、雪地等 Albedo / Normal 贴图，结合 Cook–Torrance 微表面直接光照与天空着色（`SkyShader.hlsl`）。
- **水体吸收与透射**：按水体与杂质吸收系数（Extinction）及浑浊度（Turbidity）计算能量守恒透射率：
  `FinalColor = waterColor * (1 - transmittance) + underWaterColor * transmittance`，并叠加水面扰动。
- **方差阴影贴图（VSM）**：通过深度一阶/二阶矩与滤波实现软阴影，并由 `dx12RenderGraph.h` 串联阴影与主渲染 Pass。

## 效果演示

https://github.com/user-attachments/assets/dfd41857-c09c-4986-8ccd-4d3f7fe39715

## 项目结构

- `程序化生成地形/TerrainKernel.cu` — CUDA 分层梯度噪声与地形顶点/法线生成
- `程序化生成地形/TerrainInteropManager.h` — CUDA 与 Direct3D 12 共享资源管理
- `程序化生成地形/dx12RenderGraph.h` & `RenderPipeline.h` — D3D12 渲染管线与 Pass 编排
- `程序化生成地形/shader.hlsl` / `SkyShader.hlsl` / `EdgeShader.hlsl` — 地表 PBR、VSM、水体与天空着色器
