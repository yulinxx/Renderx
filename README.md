# RenderX Library (RenderX.dll)

面向 CAD 的 2D/3D 渲染 DLL。三条硬性目标：**跨平台**、**多窗口**、**零业务耦合**。

> **当前状态（2026-08-25）**
>
> - **公共 ABI**：`include/render/renderx.h` 是唯一公共头（32 个 `rx*` 导出，
>   零 STL 跨界、全 POD、全 `static_assert` 锁尺寸、无 `bool`、句柄为
>   `enum class : uint64_t`）。旧的 `render.h` / `RenderTypes.h` /
>   `runtime_session.h` 已删除。
> - **RHI**：后端中立的命令记录模型（`ISurface` / `IGpuDevice` / `ICommandList`，
>   `(set, binding)` 描述符槽 + pushConstants，`Capabilities` 能力查询，
>   `RhiResult` 错误码）。**已实现后端：Null + OpenGL**。
>   Metal / Vulkan 的 `createDevice` 明确返回 `nullptr` 并报错，
>   **不静默回退到 Null**——回退的表现是画面全黑而调用方拿不到任何错误。
> - **RT 层**：Runtime（1 设备 + 共享资源）/ Surface（N 窗口）/ Session（N 视口）
>   已切到 `renderx.h`。多窗口共享 GPU 资源，不再是「多窗口 = 多设备」。
> - **Shader**：构建期编入二进制，运行期零文件 IO。
> - **测试**：`RenderxTests` 34/34、`RenderxGLTests` 53/53，构建零警告。
>
> **已知缺口**
>
> - 文本：字形图集未移植到新 RHI，`rxFontLoad` 返回 `ErrorUnsupportedBackend`。
> - 3D：`mesh_3d.*` 仍用独立 uniform，未并入 PushConstants 块。
> - 宿主：`UI/` 与 `Main/` 自 legacy 删除起无法构建，随 2D/3D 改造阶段修复。
>
> 完整目标设计与决策依据见 `Docs/03-渲染主链/新渲染架构.md`。

## 功能描述

- **纯描述符提交**：DLL 只回答「怎么画」——接收顶点字节流 + `DrawCommand`
  描述符，按 `sortKey` 排序合批后提交 GPU。
- **三档渲染空间**：`World`（跟随平移与缩放）、`Screen`（都不跟随）、
  `WorldPinned`（跟随平移、不跟随缩放，用于场景内定尺寸标记）。
- **多窗口**：一个 Runtime 拥有一个 GPU 设备与全部共享资源，
  N 个 Surface 对应 N 个窗口，提交与呈现分离。
- **瞬态顶点环**：双段轮转 + 帧末上传，容量不足时开临时缓冲而非回绕覆盖。
- **管线缓存**：按 (顶点格式, 空间, 拓扑, 深度/混合状态, 线宽, shader) 缓存，
  每组状态只建一次。
- **Shader 内嵌**：构建期转字节数组编入 DLL，不依赖运行目录布局。
- **日志注入**：DLL 不依赖任何日志库，出口由宿主通过 `rxLogCallback` 注入。

**不做、也不应该做的事**：几何离散化（圆/弧/椭圆/贝塞尔 → 折线）、场景图、
实体语义、图层、选择集、捕捉、单位制、拾取、文本布局排版、文件 IO。
这些都属于应用层——详见 `renderx.h` 头部的「职责边界」。

## 使用方法

完整的最小流程见下文「API 概要 / 最小使用流程」。要点：

```c
#include "render/renderx.h"   // 唯一公共头

// Runtime（设备 + 共享资源）→ Surface（窗口）→ Session（视口）
RuntimeHandle runtime = rxRuntimeCreate(&runtimeDesc);   // abiVersion 必填
SurfaceHandle surface = rxSurfaceCreate(runtime, &surfaceDesc);
SessionHandle session = rxSessionCreate(&sessionDesc);

// 每帧：BeginFrame → AllocTransient/Submit（可多次）→ EndFrame

rxSessionDestroy(session);
rxSurfaceDestroy(runtime, surface);
rxRuntimeDestroy(runtime);
```

销毁顺序是契约：Session 早于 Surface，Surface 早于 Runtime。
违反时 DLL 会记录错误并拒绝销毁（例如表面上还绑着 Session），而不是留下半死对象。

### 渲染流程

> ⚠️ **以下两节（渲染流程 / 图元操作）描述的是已删除的 legacy `render*` API**，
> 保留仅为对照历史。当前正确的用法见「API 概要」。
> 这两节会随宿主（`UI/`）切到 `rx*` 接口时一并重写——现在重写只能是对
> 尚不存在的宿主代码的猜测。

```c
// 每帧渲染的典型流程

// Step 1: 设置视图参数
float viewMatrix[9] = { /* 3x3 视图矩阵，列主序 */ };
renderSetView2D(dev, viewMatrix, viewWidth, viewHeight);

// Step 2: 添加/修改图元（详见图元操作）

// Step 3: 渲染一帧
renderFrame(dev);
```

`renderFrame` 内部执行完整的渲染流程：

1. **GPU 剔除**：将 RenderWorld 图元同步到 PersistentEntityManager，执行 GPU 视锥剔除，生成间接绘制命令
2. **可见性查询**：优先使用 GPU 剔除结果，失败时回退到 CPU 四叉树
3. **构建批次**：BatchQueue 将可见图元按图元类型和材质分组，构建间接绘制命令
4. **叠加层收集**：OverlayQueue 将叠加元素顶点数据提交
5. **命令排序与执行**：CommandEncoder 按 sortKey 排序所有绘制命令，统一绑定管线和缓冲区后执行绘制
6. **文本渲染**：TextAtlas 和 ScreenTextRenderer 渲染世界坐标文本和屏幕坐标文本
7. **呈现**：交换缓冲区完成一帧渲染

### 图元操作

#### 2D 图元管理

```c
// 添加图元
VertexP3C3 vertices[] = {
    { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f },
    { 100.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
    { 100.0f, 100.0f, 0.0f, 0.0f, 0.0f, 1.0f },
};
uint32_t idx = renderAddEntity(dev, 1001, vertices, 3, PrimitiveType::TriangleList, 0);

// 修改图元
renderModifyEntity(dev, 1001, newVertices, newVertexCount, 0);

// 删除图元
renderRemoveEntity(dev, 1001);

// 设置可见性
renderSetEntityVisibility(dev, 1001, 0);  // 0=不可见, 1=可见

// 批量更新
// renderApplyUpdates(dev, packet, packetSize);
```

#### 3D 网格管理

```c
// 注册网格
MeshId mesh = renderRegisterMesh(dev, positions, normals, indices, vertexCount, indexCount);

// 添加实例
float modelMatrix[16] = { /* 4x4 矩阵，列主序 */ };
float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
uint32_t instId = renderAddInstance(dev, mesh, modelMatrix, materialIdx, color);

// 修改实例
renderModifyInstance(dev, instId, newModelMatrix);

// 删除实例
renderRemoveInstance(dev, instId);

// 注销网格
renderUnregisterMesh(dev, mesh);
```

#### 统一几何提交

```c
// 提交单个几何图元
GeometryPrimitive prim;
prim.kind = GeometryPrimitiveKind::Polyline;
prim.desc.polyline = &polylineDesc;
renderSubmitGeometry(dev, &prim);

// 批量提交
// renderSubmitGeometries(dev, primitives, count);
```

#### 叠加层提交

```c
// 提交叠加层图元：几何形态（渲染用）× 生命周期分组（清除用）两个独立轴
// 形态: LineList / Rect / FilledRect / Marker / SnapCircle
// 分组: Ui / Preview / Control / SelectionBox / SelectionOutlines / SelectionHandles / PointMarkers / Snap
OverlayPrimitive prim;
prim.form = OverlayForm::LineList;
prim.group = OverlayGroup::Ui;
prim.payload = &polylineDesc;
prim.payloadSize = sizeof(polylineDesc);
prim.style.borderColor = 0xFFFFFFFF;
renderSubmitOverlay(dev, &prim);

// 清除叠加层（按分组）
renderClearOverlays(dev);
renderClearOverlayGroup(dev, OverlayGroup::Preview);
```

#### 材质管理

```c
// 添加材质
MaterialDesc matDesc;
matDesc.lineWidth = 2.0f;
matDesc.pointSize = 4.0f;
matDesc.color[0] = 1.0f; matDesc.color[1] = 0.0f;
matDesc.color[2] = 0.0f; matDesc.color[3] = 1.0f;
matDesc.flags = 0;
uint16_t matIdx = renderAddMaterial(dev, &matDesc);

// 更新材质
renderUpdateMaterial(dev, matIdx, &newMatDesc);
```

## 设计框架

当前分层（与代码同步）：

```
┌──────────────────────────────────────────────────────────────┐
│  公共 ABI：include/render/renderx.h（32 个 rx* 导出）          │
├──────────────────────────────────────────────────────────────┤
│  RT 层：src/rt/ + src/c_api/rxCApi.cpp                        │
│    Runtime（1 设备 + 管线缓存 / shader / 纹理 / 瞬态环）       │
│      ├─ Surface ×N（窗口表面 + 交换链）                        │
│      └─ Session ×N（视口相机 + 提交）                          │
├──────────────────────────────────────────────────────────────┤
│  RHI：src/rhi/（后端中立的命令记录模型）                       │
│    IGpuDevice / ISurface / ICommandList                       │
│    (set, binding) 描述符槽 + pushConstants + Capabilities      │
├──────────────────────────────────────────────────────────────┤
│  后端：null/（已实现）  gl/（已实现）  metal/ vulkan/（待建）   │
├──────────────────────────────────────────────────────────────┤
│  Shader 库：src/shader/ → 构建期嵌入（CMake/EmbedShaders.cmake）│
└──────────────────────────────────────────────────────────────┘
```

> ⚠️ **以下小节（C API Facade / RenderWorld / RenderGraph / CommandEncoder /
> BatchQueue / OverlayQueue / TextAtlas）描述的是已删除的 legacy 实现**，
> 保留仅为对照历史。其中：
> - `render.h` / `RenderTypes.h` / `runtime_session.h`、`RenderWorld`、
>   `RenderGraph`、`CommandEncoder`、`BatchQueue`、`OverlayQueue`、
>   `PipelineStateManager`、`PersistentEntityManager` 均已删除；
> - `TextAtlas` 已删除，待随 2D 链路改造移植到新 RHI；
> - 排序合批的职责现在在 `Session::submit`（`src/rt/rxSession.cpp`），
>   管线缓存在 `Runtime::createPipelineFromKey`（`src/rt/rxRuntime.cpp`）。
>
> 「Shader 管理」小节仍然有效。

### C API Facade

**文件**：`include/render/render.h`、`include/render/RenderTypes.h`

对外暴露的 C 接口层，所有接口使用 C 语言调用约定（`extern "C"`），便于跨语言绑定和动态库调用。API 导出宏 `RENDER_API` 支持 Windows（`__declspec(dllexport/dllimport)`）和 Linux/macOS（`__attribute__((visibility))`）。

### 渲染世界 (RenderWorld)

**文件**：`src/core/render_world.h` / `src/core/render_world.cpp`

负责管理场景中所有 2D 图元的核心组件：

- **图元生命周期**：添加、修改、删除图元，基于 `SlotMap` 实现稀疏索引到稠密索引的 O(1) 映射
- **顶点池管理**：动态分配和释放顶点数据空间，支持脏区增量上传
- **四叉树空间分区**：实现高效的视锥体可见性查询（CPU 端回退方案）
- **材质管理**：添加和更新渲染材质

### 渲染图 (RenderGraph)

**文件**：`src/core/render_graph.h` / `src/core/render_graph.cpp`

Phase 4 引入的显式 Pass 调度层：

- 按添加顺序依次执行渲染 Pass
- 支持每个 Pass 的 `onSetup`（状态设置）和 `onExecute`（实际渲染）回调
- 支持 Pass 级别的启用/禁用控制
- 声明资源输入/输出，为后续自动屏障管理预留接口
- 2D 模式 Pass 编排：FrameSetup → SceneEnv → World2DCollect → OverlayCollect → CommandExecute → Text
- 3D 模式 Pass 编排：FrameSetup3D → Mesh3D

### 命令编码器 (CommandEncoder)

**文件**：`src/core/command_encoder.h` / `src/core/command_encoder.cpp`

Phase 3 引入的统一命令收集与排序组件：

- 统一收集 World2D 和 Overlay 的绘制命令到同一条链路
- 基于 64-bit `BatchKey` 排序（空间 → Z序 → 图元类型 → 材质），减少状态切换
- 支持通过 `PipelineStateManager` 复用管线缓存
- 支持通过 `DrawBatcher` 实现 Overlay 路径的 MDI 合批

### 批量队列 (BatchQueue)

**文件**：`src/core/batchqueue.h` / `src/core/batch_queue.cpp`

2D 图元的批量绘制管理：

- 将可见图元按图元类型和材质分组，构建间接绘制命令（`glDrawArraysIndirect`）
- Dirty 范围合并，只增量上传修改过的顶点区间
- 顶点缓冲区动态扩容，支持首次全量上传和后续增量更新

### 叠加层队列 (OverlayQueue)

**文件**：`src/core/overlay_queue.h` / `src/core/overlay_queue.cpp`

管理所有叠加在场景之上的 UI 元素：

- 十字准星、捕捉指示器、预览线、控制线、点标记、选择框、选择手柄
- 支持统一 API（`submitOverlay`）和旧 API 的兼容封装
- 几何形态（`OverlayForm`，渲染用）× 生命周期分组（`OverlayGroup`，清除用）两轴分离；支持按分组增量清除（`clearGroup` / `renderClearOverlayGroup`）
- 合并所有子项到统一顶点缓冲区，批量渲染

### 文本图集 (TextAtlas)

**文件**：`src/core/text_atlas.h` / `src/core/text_atlas.cpp`

基于 stb_truetype 的字体光栅化和纹理图集管理：

- 动态加载 TTF/OTF 字体
- Glyph 缓存与图集打包（2048×2048 像素图集）
- 构建文本四边形（带纹理坐标和颜色）
- 支持多种字体大小和对齐方式

### Shader 管理

**文件**：`src/shader/shaderLibrary.h` / `src/shader/shaderLibrary.cpp`
**构建期代码生成**：`CMake/EmbedShaders.cmake` → `<build>/generated/shaderBlobs.cpp`

Shader 在构建期转成字节数组编入 DLL，运行期没有任何文件 IO：

- `src/shader/*.vert|frag|comp` 由 `EmbedShaders.cmake` 转成 `unsigned char[]`，
  语言按扩展名推断（`.vert/.frag/.comp` → GLSL，`.spv` → SPIR-V，`.metal` → MSL，`.metallib` → MetalLib）
- shader 文件是 `add_custom_command` 的 `DEPENDS`，改动即触发重新生成与重新编译
- 运行期通过 `shader::find(name, language, &blob)` / `shader::glslSource(name)` 按文件名查表；
  后端拿到的 `PipelineDesc::vertexShader` 既可以是 GLSL 源码本身，也可以是库中的文件名（如 `"world_p3c3.vert"`）
- 新增 shader 只需把文件加进 `CMakeLists.txt` 的 `RENDERX_SHADER_SOURCES` 列表，无需改任何 C++ 代码

替换了旧的 `shaders.h` / `shaders.cpp` 方案。旧方案有三个实际故障：macOS `.app` bundle 下
从可执行文件路径推导 shader 目录失败导致视口全黑（见 `Docs/Mac渲染.md` §4）；shader 文件不是
构建依赖，调试时跑的是旧 shader；文本加载无法承载 Vulkan/Metal 需要的 SPIR-V 与 metallib。

## 依赖库

| 依赖库 | 说明 |
|--------|------|
| OpenGL | 跨平台图形 API，Windows 使用 opengl32，Linux/macOS 使用 OpenGL::GL |
| Log | SanYi CAD 项目内部日志库（`../Log/Log/Include`） |
| stb | Header-only 字体渲染库（stb_truetype），用于字体光栅化 |

## 渲染精度优化

### 问题描述

当 CAD 场景包含大坐标数据（如 DXF 导入的工程图纸，坐标量级可达 10^5 ~ 10^6），在高倍放大时会出现以下渲染异常：

- **虚线现象**：实线显示为断断续续的虚线
- **图元消失**：部分线段或图形在高倍放大时不可见
- **跟随异常**：平移视图时图元不跟随相机移动

### 根因分析

问题源于 **float32 精度限制**。顶点着色器中的视图矩阵乘法：

```glsl
vec3 pos = uViewMatrix * vec3(aPosition.xy, 1.0);
// 展开为：ndcX = scaleX * worldX + tx
```

当 `worldX` 和 `camX` 都很大（如 200000）时：
- `scaleX * worldX` ≈ 200000 × scaleX（极大值）
- `tx = -scaleX * camX` ≈ -200000 × scaleX（极大值）
- 两者差值应为小值（如 0.1），但 float32 在 200000 量级的精度仅约 0.024

**灾难性抵消**（Catastrophic Cancellation）导致相邻顶点坍缩到同一 NDC 位置，形成零长度线段，OpenGL 无法渲染。

### 解决方案：双层相机相对渲染

采用 **细分阶段 + 着色器阶段** 双层精度优化策略：

1. **细分阶段 double 精度减法**：在 `tessellatePolyline`/`tessellateCircle`/`tessellateArc`/`tessellateEllipse` 中，以 `double` 精度减去相机中心后再转 `float`，使传入 GPU 的顶点坐标保持在相机附近的小数值范围
2. **着色器每帧减去相机中心**：在顶点着色器中执行 `relPos = aPosition.xy - uCameraCenter`（此时 `aPosition` 已是相机附近的小值 float，减法误差极小）
3. **World2D 使用纯缩放矩阵**：移除视图矩阵中的平移分量，避免灾难性抵消
4. **相机中心作为 uniform 传入**：每帧更新，确保平移/缩放时精度正确

#### 着色器实现（legacy `scene_2d.vert`，已删除）

> 现状：`uCameraCenter` 这一层已不存在。当前 RT 路径的世界空间 shader
> （`world_p3c3.vert` / `world_p3c4.vert`）只有一个 `uniform mat4 uView`，
> 相机相对偏移完全在 CPU 侧的离散化阶段以 double 精度减去（按
> `Docs/03-渲染主链/新渲染架构.md` §1，离散化归应用层的 `RenderSceneBuilder`）。
> 下面这段是 legacy 2D 路径的做法，保留作为精度问题的说明。

```glsl
uniform mat3 uViewMatrix;
uniform vec2 uCameraCenter;

void main()
{
    vec2 relPos = aPosition.xy - uCameraCenter;
    vec3 pos = uViewMatrix * vec3(relPos, 1.0);
    gl_Position = vec4(pos.xy, aPosition.z, 1.0);
}
```

#### 渲染流程

```
┌─────────────────────────────────────────────────────────────────┐
│                        CPU 端（每帧）                             │
│                                                                 │
│  1. ViewCamera2D::computeViewMatrix() 计算视图矩阵（含平移）      │
│  2. 提取相机中心：camX = -viewMatrix[6] / viewMatrix[0]          │
│                    camY = -viewMatrix[7] / viewMatrix[4]         │
│  3. renderSetCameraCenter(dev, camX, camY) 存储相机中心           │
│  4. renderFrame() → commandEncoder.execute() 传递相机中心         │
├─────────────────────────────────────────────────────────────────┤
│                        GPU 端（顶点着色器）                       │
│                                                                 │
│  World2D:                                                       │
│    uViewMatrix = 纯缩放矩阵（无平移）                             │
│    uCameraCenter = 实际相机中心                                   │
│    relPos = worldPos - camCenter                                 │
│    ndc = scale * relPos                                          │
│                                                                 │
│  SceneEnv:                                                      │
│    uViewMatrix = 完整视图矩阵（含平移）                           │
│    uCameraCenter = (0,0)                                         │
│    relPos = worldPos                                             │
│    ndc = fullMatrix * relPos                                     │
└─────────────────────────────────────────────────────────────────┘
```

#### 精度对比

| 场景 | 原始方案（float32） | 着色器减法（float32） | 细分阶段减法（double→float） |
|------|---------------------|------------------------|------------------------------|
| worldX=200000, camX=199999, scale=1000 | 灾难性抵消，误差 ~24 | 减法误差 ~0.024，放大后 ~24 | double 减法无误差，float 误差 ~0 |
| worldX=200000, camX=199999.5, scale=1000 | 完全坍缩，误差 ~1000 | 减法误差 ~0.024，放大后 ~24 | double 减法无误差，float 误差 ~0 |

> **实现说明**：细分阶段（`tessellatePolyline` 等）已实现 double 精度减去相机中心后再转 float，从根源消除精度丢失。着色器中的 `uCameraCenter` 减法作为第二层保障，此时 `aPosition` 已是相机附近的小值 float，减法误差可忽略。两层配合下，即使坐标量级达 10^6 也能保持像素级精度。

### 新增/修改文件列表

| 文件 | 修改内容 |
|------|----------|
| `src/c_api/rendercapiinternal.h` | 添加 `double cameraCenter[2]` 到 RenderDevice |
| `include/render/render.h` | 声明 `renderSetCameraCenter()` API |
| `src/c_api/render_c_api_frame.cpp` | 实现 `renderSetCameraCenter()`；`tessellatePolyline`/`tessellateCircle`/`tessellateArc`/`tessellateEllipse` 增加 `cameraCenter` 参数，double 精度减法后再转 float |
| `src/shader/scene_2d.vert` | 添加 `uCameraCenter` uniform，相机相对变换 |
| `src/core/command_encoder.h/.cpp` | `execute()` 增加相机中心参数，World2D 使用纯缩放矩阵 |
| `src/core/scene_env.cpp` | 设置 `uCameraCenter = (0,0)`；修复 `asTriangles` 自动推断逻辑（`triangleFlags=null` 时默认 `false`，不再按 `vertexCount%3==0` 误判） |
| `src/rhi/rhigl.cpp` | 移除全局 `GL_LINE_SMOOTH`（与 MSAA 冲突导致线条消失）；`setLineWidth()` 增加范围钳制（查询 `GL_LINE_WIDTH_RANGE`） |
| `src/rhi/rhi_gl.h` | 添加 `m_minLineWidth`/`m_maxLineWidth` 成员 |
| `src/platform/gl_loader.h` | 添加 `GetFloatv`/`Hint` 函数指针；添加 `GL_LINE_WIDTH_RANGE`/`GL_ALIASED_LINE_WIDTH_RANGE`/`GL_NICEST`/`GL_LINE_SMOOTH_HINT` 常量 |
| `src/platform/glloader.cpp` | 初始化 `GetFloatv`/`Hint` 函数指针 |
| `UI/.../RenderWidget.cpp` | `setViewMatrix()` 和 `initializeGL()` 中调用 `renderSetCameraCenter()`；macOS 显式请求 GL 4.1 CoreProfile（原先请求 4.6 被静默降级）；`setSceneCommands()` 使用 `RenderCommand::primitiveType` 替代硬编码 `LineList` |
| `UI/Common/Include/Render/RenderTypes.h` | 添加 `RenderPrimitiveType` 枚举和 `RenderCommand::primitiveType` 字段（默认 `LineStrip`） |

## 跨平台渲染兼容性

### 问题描述

同一套渲染代码在 Windows 和 macOS 上表现不一致：

- **macOS 网格线渲染异常**：网格线缺失或被错误渲染为填充三角形
- **六边形缩放后交替缺线**：6 条边的多边形缩放后只显示 3 条边（间隔一条缺一条）
- **缩放后线条消失**：部分线段在特定缩放级别不可见
- **线宽跨平台不一致**：Windows 上线宽正常，macOS 上全部退化为 1px

### 根因分析

| 问题 | 根因 | 影响 |
|------|------|------|
| 六边形交替缺线 | `setSceneCommands` 硬编码 `PrimitiveType::LineList`，6 顶点只画 3 条边 | Windows + macOS |
| 缩放后线条消失 | `GL_LINE_SMOOTH` 与 4x MSAA 叠加，覆盖率丢弃低阈值片段 | Windows + macOS |
| 网格线误渲染 | `asTriangles` 自动推断 `vertexCount%3==0`，6 顶点网格线被误判为三角形 | Windows + macOS |
| Mac 线宽退化 | macOS CoreProfile 下 `GL_LINE_WIDTH_RANGE=[1,1]`，`glLineWidth` 无钳制 | 仅 macOS |
| Mac GL 版本异常 | 请求 GL 4.6 被静默降级为 4.1，上下文配置可能异常 | 仅 macOS |

### 解决方案

#### 1. 图元拓扑类型修复

给 `Render::RenderCommand` 添加 `RenderPrimitiveType` 枚举字段（默认 `LineStrip`），`setSceneCommands` 根据该字段映射到 `render::PrimitiveType`，不再硬编码 `LineList`。

```
RenderPrimitiveType 枚举:
  Points → PointList
  Lines → LineList       (独立线段，每 2 顶点一条)
  LineStrip → LineStrip   (连续线段，默认值)
  LineLoop → LineLoop     (闭合线段)
  Triangles → TriangleList
  TriangleFan → TriangleFan
```

#### 2. GL_LINE_SMOOTH 移除

移除全局 `glEnable(GL_LINE_SMOOTH)`。原因：
- `GL_LINE_SMOOTH` 基于覆盖率的抗锯齿与 4x MSAA 叠加时，会在特定缩放级别丢弃低于覆盖阈值的线段片段
- MSAA 已提供足够的多重采样抗锯齿，无需额外的 `GL_LINE_SMOOTH`
- macOS CoreProfile 下 `GL_LINE_SMOOTH` 仅对 width=1.0 的线有效，实际效果有限

#### 3. 线宽范围钳制

`GLDevice::initialize()` 时查询 `GL_LINE_WIDTH_RANGE`，`setLineWidth()` 钳制到 GPU 支持范围：
- macOS CoreProfile：`[1.0, 1.0]`（OpenGL 规范限制，所有线宽退化为 1px）
- Windows CompatibilityProfile：可能支持更宽线宽（取决于驱动）

> **macOS 线宽限制说明**：macOS OpenGL CoreProfile 将 `glLineWidth` 钳制为 1.0 是平台规范限制。若需在 Mac 上实现粗线，需使用 geometry shader 或 triangle strip 模拟。

#### 4. asTriangles 安全默认

`SceneEnv::setGeometryEx` 中 `triangleFlags=null` 时，`asTriangles` 默认为 `false`（线段渲染），不再按 `vertexCount%3==0` 自动推断。

#### 5. macOS OpenGL 版本

`RenderWidget` 显式请求 GL 4.1 CoreProfile（macOS 最高支持版本），与 `main.cpp` 全局设置保持一致。Windows 仍使用 4.6 CompatibilityProfile。

| 平台 | GL 版本 | Profile | 用途 |
|------|---------|---------|------|
| macOS | 4.1 | CoreProfile | macOS 最高支持，避免静默降级 |
| Windows | 4.6 | CompatibilityProfile | 兼容 3D 端固定管线 |

## 架构优化

### STL 导入路径统一化

**问题描述：** STL 导入原来直接使用 `StlLoader`，绕过了 `FileIOManager::importToIR()` 中立 IR 路径，与 DXF/STEP 等格式的导入架构不一致。

**解决方案：** 创建 `StlParser` 实现 `IFileParser` 接口，将 STL 解析结果输出为中立 IR（`FioParseResult`），再通过 `FioEntityConverter` 转换为 `SyMeshEntity`。

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `FileIO/FileIO/Include/FileIO/Parsers/StlParser.h` | 新建：STL 解析器声明 |
| `FileIO/FileIO/Src/Parsers/StlParser.cpp` | 新建：STL 解析器实现（支持 ASCII/Binary） |
| `FileIO/FileIO/Src/FileParserFactory.cpp` | 注册 STL 解析器 |
| `Main/Src/Import/Readers/StlImportReader.cpp` | IR 路径优先 + 回退旧路径 |
| `Main/Src/Import/FioEntityConverter.cpp` | 添加 `Mesh3D` case 转换 |

### 3D 导入 Undo 支持

**问题描述：** 3D 导入直接调用 `SceneManager3D::addEntity()`，不支持 Undo/Redo。

**解决方案：** 创建 `AddMeshCommand3D` 撤销命令，扩展 `SceneEditService3D::addEntities()` 方法，使 3D 导入通过编辑服务添加图元时自动创建撤销命令。

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `Engine3D/Include/Engine3D/Edit/SceneUndoCommands3D.h` | 添加 `AddMeshCommand3D` 声明 |
| `Engine3D/Src/Edit/SceneUndoCommands3D.cpp` | 实现 `AddMeshCommand3D` |
| `UI3D/Include/UI3D/Edit/SceneEditService3D.h` | 添加 `addEntities()` 声明 |
| `UI3D/Src/Edit/SceneEditService3D.cpp` | 实现 `addEntities()` |
| `UI3D/Include/UI3D/Edit/UndoCommands3D.h` | 添加 `makeAddMesh()` 声明 |
| `UI3D/Src/Edit/UndoCommands3D.cpp` | 实现 `makeAddMesh()` 包装器 |
| `Main/Src/Import/ImportService.h/.cpp` | 3D 导入通过 `SceneEditService3D` |

### 统一渲染管线

**问题描述：** 3D 渲染使用 OpenGL 固定管线（`glBegin/glEnd`），与 2D 的现代 RHI 管线架构不一致。

**解决方案：** 创建 `RenderWorld3D` 作为 3D 场景数据管理组件，为后续完全迁移到统一管线奠定基础。

**后续进展（已完成）：** `UI3D` 的 `RenderWidget3D` 已彻底移除固定管线（`glBegin/glEnd/glMatrixMode/glLight*` 等），全部改写为现代 OpenGL：`QOpenGLShaderProgram`（GLSL 330 core） + `QOpenGLVertexArrayObject`/`QOpenGLBuffer` 绘制网格/坐标轴/网格体（Phong 光照着色器）与选中线框。上下文统一为 CoreProfile：Windows/Linux 4.6，macOS 4.1（系统最高支持），无 `glBegin/glEnd` 等固定管线调用。

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `Renderx/src/core/render_world_3d.h` | 新建：3D 渲染世界声明 |
| `Renderx/src/core/render_world_3d.cpp` | 新建：3D 渲染世界实现 |
| `UI3D/Src/Render/RenderWidget3D.cpp` | 3D 视图渲染器：固定管线 → 现代 GL（shader/VBO/VAO） |
| `UI3D/Include/UI3D/Render3D/RenderWidget3D.h` | 添加现代 GL 成员与辅助方法声明 |

**RenderWorld3D 接口：**

```cpp
class RenderWorld3D {
public:
    bool initialize(uint32_t initialVertexCapacity = 65536,
                    uint32_t initialIndexCapacity = 65536);
    void shutdown();

    void addEntity(EntityId id, const VertexP3N3* vertices, uint32_t vertexCount,
                   const uint32_t* indices, uint32_t indexCount,
                   uint16_t materialIdx);
    void removeEntity(EntityId id);
    void clear();

    const EntityEntry3D* getEntityEntries() const;
    uint32_t getEntityCount() const;
    // ...
};
```

### DisplayCache 3D 化

**问题描述：** 2D 有 `DisplayCache` 分块+LOD 缓存机制，3D 缺少类似机制。

**解决方案：** 创建 `DisplayCache3D` 组件，支持八叉树空间分块和基于距离的 LOD 选择。

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `Engine3D/Include/Engine3D/Render/DisplayCache3D.h` | 新建：3D 显示缓存声明 |
| `Engine3D/Src/Render/DisplayCache3D.cpp` | 新建：3D 显示缓存实现 |

**DisplayCache3D 特性：**
- 八叉树空间分块（可配置块大小）
- 基于距离的 LOD 选择（可配置 LOD 层级）
- 增量更新（仅重建脏块）

### 异步导入

**问题描述：** 大文件导入时 UI 阻塞。

**解决方案：** 在 `ImportService` 中添加 `importAsync()` 方法，Phase 1-2（格式检测+解析）在后台线程执行，Phase 3-5（文档构建+UI 刷新）回到主线程。

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `Main/Src/Import/ImportService.h` | 添加 `importAsync()` 声明 |
| `Main/Src/Import/ImportService.cpp` | 实现 `importAsync()` |

**使用方式：**

```cpp
importService->importAsync(context, options, [](const ImportResult& result) {
    // 导入完成回调（在主线程执行）
    if (result.success) {
        // 处理成功
    }
});
```

### 接口统一

**确认：** `SceneManager3D` 已实现 `ISceneManager` 接口，2D/3D 场景管理器接口统一完成。

## 依赖库安装方法

### Windows

OpenGL 通过系统自带的 `opengl32.lib` 自动链接，无需额外安装。

stb 库支持两种获取方式：
- **vcpkg**：通过环境变量 `VCPKG_DIR` 指定路径，CMake 自动从 `installed/x64-windows/include` 查找
- **手动安装**：将 stb 头文件放置到项目可搜索的路径

Log 库为项目内部库，确保 `../Log/Log/Include` 目录存在即可。

### Linux / macOS

```bash
# OpenGL
find_package(OpenGL REQUIRED)
# 系统通常已自带，开发环境需安装 mesa/libglvnd 等开发包
# Ubuntu: sudo apt install libgl1-mesa-dev
# macOS: 系统框架自带
```

## 构建配置

### CMake 配置说明

```cmake
cmake_minimum_required(VERSION 4.3)
project(RenderX VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
```

**编译器配置**：
- MSVC：`/W4 /utf-8`，禁用 `_CRT_SECURE_NO_WARNINGS` 和类型转换警告
- GCC/Clang：`-Wall -Wextra -Wpedantic`，禁用类型转换相关警告

**链接依赖**：

| 平台 | OpenGL 链接 | 其他 |
|------|------------|------|
| Windows | `opengl32` | - |
| Linux | `OpenGL::GL` | `stdc++fs`（GCC） |
| macOS | `OpenGL::GL` | - |

**输出配置**：
- Debug 版本添加 `_d` 后缀（`SanYiRender_d.dll`）
- 公开头文件安装到 `include/`
- 动态库安装到 `bin/`，静态库安装到 `lib/`

### Shader 嵌入（不再有文件复制）

Shader 在构建期编入 DLL，构建后输出目录中不再有 `.vert/.frag/.comp` 文件，运行期也不再读盘。
详见上文「Shader 管理」。当前嵌入 27 个文件：

| 类别 | 文件名 |
|------|--------|
| 世界空间图元 | `world_p3c3.vert`、`world_p3c3.frag`、`world_p3c4.vert`、`world_p3c4.frag` |
| 屏幕空间图元 | `screen_p3c3.vert`、`screen_p3c4.vert`、`screen_p3c4.frag` |
| 世界锚定定尺寸 | `world_pinned_p3o2c4.vert`（片段复用 `world_p3c4.frag`） |
| 点图元 | `world_point_p3c3.vert`、`world_point_p3c4.vert`、`screen_point_p3c3.vert`、`screen_point_p3c4.vert`、`point_p3c3.frag`、`point_p3c4.frag` |
| 屏幕空间纹理 | `screen_tex_p2t2c4.vert`、`screen_tex_p2t2c4.frag` |
| 3D 网格 | `mesh_3d.vert`、`mesh_3d.frag`、`mesh_3d_instanced.vert` |
| 文本 | `text_sdf.vert`、`text_sdf.frag`、`text_screen.vert`、`text_screen.frag` |
| 高亮 | `highlight_3d.vert`、`highlight_3d.frag` |
| GPU 剔除 | `culling.comp` |

此外 `rx_push_constants.glsl` 是**被 include 的片段**，不作为独立 shader 嵌入，
但声明为构建依赖。它是全部 RT shader 共用的 std140 pushConstant 块的唯一声明处
（`uView` / `uViewport` / `uPointSize` / `uSdfScale`，共 80 字节），
与 C++ 侧 `Render::RT::detail::PushConstants` 逐字节对应并有 `static_assert` 锁定。
展开由 `CMake/EmbedShaders.cmake` 在构建期完成（单文件、双引号、同目录）。
之所以不允许各 shader 各抄一份：std140 布局不匹配不产生编译错误，只会算出错误偏移。

点图元必须有独立的顶点着色器：点尺寸是像素量，只能写 `gl_PointSize`，
不能用顶点几何表达（否则缩放时点会跟着变大）。GL 后端在设备创建时统一开启
`GL_PROGRAM_POINT_SIZE`——桌面核心 profile 下不开这个开关，
shader 里写的 `gl_PointSize` 会被静默忽略。

已删除：`scene_2d.*`、`overlay.*`、`overlay_screen.*`、`bitmap.*`——它们只服务于已删除的
legacy 渲染路径。世界/屏幕空间图元的 shader 是从 `rendererRuntime.cpp` 中 11 段内联
GLSL 字符串字面量提取出来的独立文件。`world_point_p3c3.frag` 已更名为 `point_p3c3.frag`：
点的圆形裁剪只发生在屏幕上，与渲染空间无关。

### 字体文件

DLL 不再从磁盘读取字体。原先 `renderCreateDevice` 用 `std::filesystem` 从可执行文件路径推导
目录再读 `default_screen_font.ttf`，这让 DLL 依赖运行目录布局，在 macOS `.app` bundle 下极易失效。
现改为由宿主通过 `rxFontLoad(runtime, fontData, dataSize, pixelHeight)` 以内存数据注入。
`src/res/default_screen_font.ttf` 保留在仓库中供宿主取用，但不再由构建复制或安装。

### 测试构建

```bash
# 需要 GTest 支持
find_package(GTest QUIET)
option(BUILD_RENDERX_TESTS "Build Renderx unit tests" ON)
add_subdirectory(Test)
```

## API 概要

`include/render/renderx.h` 是唯一公共头，共 32 个 `rx*` 导出。
旧的 `render.h`（62 个 `render*` 函数）与 `runtime_session.h`（26 个函数）已删除：
前者把图元语义、几何离散化、场景图、文本排版全部塞进了渲染 DLL；
后者与 `renderx.h` 在 `namespace Render::RT` 里有 11 个同名但不兼容的类型。

### 版本与后端

| 函数 | 说明 |
|------|------|
| `rxGetAbiVersion` | 返回 DLL 编译时的 ABI 版本，与头里的 `RENDERX_ABI_VERSION` 比对 |
| `rxResultName` / `rxBackendName` | 结果码 / 后端名的字符串化 |
| `rxIsBackendAvailable` | 某后端在当前构建与当前机器上是否可用 |

### Runtime：进程内的 GPU 与共享资源

| 函数 | 说明 |
|------|------|
| `rxRuntimeCreate` / `rxRuntimeDestroy` | 一个 Runtime = 一个 GPU 设备 + 全部共享资源。ABI 版本不匹配直接失败 |
| `rxRuntimeGetCapabilities` | 后端能力查询（线宽上限、纹理上限、计算/间接绘制支持等），必须先查再决定渲染策略 |
| `rxBufferCreate` / `rxBufferDestroy` / `rxBufferUpload` | 长期存活的缓冲。世代式句柄，销毁后旧句柄立即失效 |
| `rxPipelineCreate` | 自定义管线，返回 `uint16` 索引（0 = 失败）。相同状态命中缓存返回同一索引 |
| `rxPipelineGetDefault` | 取 15 条内建管线之一的索引 |
| `rxTextureCreate` / `rxTextureDestroy` / `rxTextureUpdate` | RGBA8 纹理。销毁时其绑定组一并失效 |
| `rxMaterialAdd` / `rxMaterialUpdate` | 线宽/点大小/颜色。索引 0 保留为「无材质」 |
| `rxFontLoad` | 以内存数据注入字体（DLL 不做文件 IO）。**当前返回 `ErrorUnsupportedBackend`**，见下 |

### Surface：窗口表面

| 函数 | 说明 |
|------|------|
| `rxSurfaceCreate` / `rxSurfaceDestroy` | 同一 Runtime 可创建任意多个，共享全部 GPU 资源。这是多窗口的正确形态 |
| `rxSurfaceResize` | 重建交换链。尺寸为 0（最小化）时安全跳过 |

### Session：一个视口的相机与提交

| 函数 | 说明 |
|------|------|
| `rxSessionCreate` / `rxSessionDestroy` | 1 Session 绑 1 Surface；同一表面上的第二个 Session 会被拒绝 |
| `rxSessionSetClearColor` / `rxSessionSetViewMatrix` | 清屏色与视图矩阵（列主序 4x4） |
| `rxSessionBeginFrame` | 获取后备缓冲（GL 在此 makeCurrent）并开启 render pass。返回 `ErrorSurfaceOutOfDate` 时 resize 后重试本帧 |
| `rxSessionAllocTransient` | 分配本帧顶点内存，只在 Begin/End 之间有效 |
| `rxSessionSubmit` | 提交一批 `DrawCommand`；同一帧内可多次调用。DLL 按 `sortKey` 稳定排序后合批 |
| `rxSessionEndFrame` | 结束 render pass、提交命令、呈现 |
| `rxSessionQueryVisibility` | CPU 侧 AABB 与视口矩形相交（纯几何，不涉及 GPU） |
| `rxSessionGetStats` | 本帧统计：绘制调用、三角/线/点数、管线切换、瞬态用量、GPU 内存 |

### 工具

| 函数 | 说明 |
|------|------|
| `rxMakeSortKey` | `layer(8) \| transparent(8) \| depth(16) \| seq(16)`，高位优先。覆盖层约定 `layer=200, transparent=1` |

### 最小使用流程

```cpp
#include "render/renderx.h"
using namespace Render::RT;

RuntimeDesc rd{};
rd.abiVersion = RENDERX_ABI_VERSION;   // 必填，否则创建失败
rd.backend = Backend::Auto;
rd.logCallback = &myLogSink;           // DLL 不依赖任何日志库，出口由宿主注入
RuntimeHandle runtime = rxRuntimeCreate(&rd);

SurfaceDesc sd{};
sd.windowKind = NativeWindowKind::ForeignGlContext;  // Qt QOpenGLWidget 场景
sd.width = w; sd.height = h;
SurfaceHandle surface = rxSurfaceCreate(runtime, &sd);

SessionDesc ssd{};
ssd.runtime = runtime; ssd.surface = surface;
SessionHandle session = rxSessionCreate(&ssd);

// ---- 每帧 ----
if (rxSessionBeginFrame(session) == RxResult::Ok)
{
    TransientAlloc alloc{};
    rxSessionAllocTransient(session, vertexBytes, &alloc);
    std::memcpy(alloc.cpuPtr, vertices, vertexBytes);

    DrawCommand cmd{};
    cmd.vertexBuffer = alloc.buffer;
    cmd.vertexOffset = alloc.offset;      // 字节偏移，与 alloc.offset 同一坐标系
    cmd.vertexCount  = vertexCount;
    cmd.topology     = PrimitiveTopology::LineStrip;
    cmd.space        = RenderSpace::World;
    cmd.vertexFormat = VertexFormat::P3C4;
    cmd.indexType    = IndexType::None;
    cmd.sortKey      = rxMakeSortKey(10, 0, 0, seq++);

    DrawPacket packet{};
    packet.commands = &cmd;
    packet.commandCount = 1;
    std::memcpy(packet.viewMatrix, viewMatrix, sizeof(viewMatrix));
    rxSessionSubmit(session, &packet);

    rxSessionEndFrame(session);
}
```

### 三档渲染空间

| `RenderSpace` | 跟随平移 | 跟随缩放 | 顶点格式 | 用途 |
|---------------|----------|----------|----------|------|
| `World` | 是 | 是 | P3C3 / P3C4 | 常规图元 |
| `Screen` | 否 | 否 | P3C3 / P3C4 / P2T2C4 | HUD、标尺、屏幕角标 |
| `WorldPinned` | 是 | **否** | P3O2C4（锚点 + 像素偏移） | 场景内定尺寸标记：箭头、符号、标注框、引线端点 |

`WorldPinned` 的换算在顶点着色器内完成（`clip.xy += offsetPx * (2.0/uViewport) * clip.w`，
乘 `clip.w` 抵消透视除法，因此偏移恒等于 N 个像素）。
**拾取判定必须用同一公式**，否则视觉与命中区会随缩放错位。详见
`Docs/03-渲染主链/新渲染架构.md` §15。

### 当前缺口

- **文本**：字形图集尚未移植到新 RHI，`rxFontLoad` 明确返回 `ErrorUnsupportedBackend`
  并记录错误，不静默成功。
- **3D（P3N3）**：`mesh_3d.*` 仍用独立 uniform，未并入 PushConstants 块；
  `VertexFormat::P3N3` 的管线解析会明确失败并报错，不提供半实现路径。
- **Metal / Vulkan**：`RHI::createDevice` 对这两个后端返回 `nullptr` 并报错，
  不静默回退到 Null（回退的表现是画面全黑而调用方拿不到任何错误）。

### 渲染流程说明

```
rxSessionBeginFrame
  ├─ ISurface::acquireNextImage()      GL 后端在此 makeCurrent（宿主不需要自己做）
  ├─ IGpuDevice::beginFrame()          取本帧命令记录器
  ├─ TransientRing::beginFrame()       双段轮转（按 Runtime 计数，每帧只切一次）
  └─ ICommandList::beginRenderPass()   clear 在此一次性给定

rxSessionSubmit（同一帧可多次）
  ├─ TransientRing::flush()            顶点数据必须在任何绘制之前落到 GPU
  ├─ 按 sortKey 稳定排序               同键保持提交顺序，覆盖层叠放才可预测
  └─ 逐条：bindPipeline → pushConstants → bindVertexBuffer
           → bindBindGroup(纹理) → draw / drawIndexed

rxSessionEndFrame
  ├─ ICommandList::endRenderPass()
  ├─ IGpuDevice::submitFrame()         只提交，不呈现
  └─ ISurface::present()               GL: swapBuffers / Metal: presentDrawable / VK: queuePresent
```

「提交」与「呈现」分离是多窗口共享资源的前提：N 个 Session 可以各自录制并提交后
再统一呈现，设备不与任何一个窗口绑死。

绑定状态在帧内做冗余消除：管线、顶点缓冲（含偏移）、纹理绑定组、pushConstant
块各自只在变化时重新提交；换管线后统一重推一次，而不是逐后端推理哪些绑定还有效。

### Shader 列表

所有 RT shader 通过 `#include "rx_push_constants.glsl"` 共用同一个 std140
pushConstant 块（`uView` / `uViewport` / `uPointSize` / `uSdfScale`）。

| 内建管线 | 顶点 | 片段 | 用途 |
|----------|------|------|------|
| WorldLine / WorldTri | `world_p3c3.vert` | `world_p3c3.frag` | 世界空间图元（P3C3） |
| WorldPoint | `world_point_p3c3.vert` | `point_p3c3.frag` | 世界空间圆点（像素定尺寸，片段做圆形裁剪） |
| WorldLine4 / WorldTri4 | `world_p3c4.vert` | `world_p3c4.frag` | 世界空间图元（P3C4，带 alpha） |
| WorldPoint4 | `world_point_p3c4.vert` | `point_p3c4.frag` | 世界空间圆点（带 alpha） |
| ScreenLine / ScreenTri | `screen_p3c3.vert` | `world_p3c3.frag` | 屏幕空间图元（P3C3） |
| ScreenPoint | `screen_point_p3c3.vert` | `point_p3c3.frag` | 屏幕空间圆点 |
| ScreenLine4 / ScreenTri4 | `screen_p3c4.vert` | `screen_p3c4.frag` | 屏幕空间图元（P3C4，带 alpha） |
| ScreenPoint4 | `screen_point_p3c4.vert` | `point_p3c4.frag` | 屏幕空间圆点（带 alpha） |
| ScreenTextured | `screen_tex_p2t2c4.vert` | `screen_tex_p2t2c4.frag` | 屏幕空间纹理（字形图集、位图） |
| WorldPinnedLine / WorldPinnedTri | `world_pinned_p3o2c4.vert` | `world_p3c4.frag` | 世界锚定 + 屏幕定尺寸（P3O2C4） |

尚未接入 RT 默认管线（随对应阶段启用）：

| Shader | 文件 | 状态 |
|--------|------|------|
| Mesh3D / Mesh3DInstanced / Highlight3D | `mesh_3d.*`、`mesh_3d_instanced.vert`、`highlight_3d.*` | 仍用 `uModelMatrix`/`uViewMatrix`/`uProjMatrix` 独立 uniform，未并入 PushConstants；3D 收口阶段处理 |
| TextSDF / TextScreen | `text_sdf.*`、`text_screen.*` | 字形图集未移植到新 RHI，`rxFontLoad` 当前返回 `ErrorUnsupportedBackend` |
| Culling | `culling.comp` | GPU 视锥剔除，RT 当前走 CPU 的 `rxSessionQueryVisibility` |

> 所有 shader 都显式标注 `layout(location = N)`：Apple 的 GLSL 编译器在缺省时会乱序分配
> attribute slot，导致颜色/坐标错位（见 `Docs/Mac渲染.md` §9）。

## 版本信息

- **当前版本**：2.0.0
- **C++ 标准**：C++17
- **构建类型**：支持 Debug（`_d` 后缀）和 Release
- **编译器支持**：MSVC、GCC、Clang