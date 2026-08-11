# SanYiRender Library (SanYiRender.dll)

工业级 2D/3D 渲染库，为 SanYi CAD 项目提供高性能的渲染解决方案。

> **当前定位（2026-08-10 复核 + 编译验证）**：多后端渲染运行时。`BackendType` 中的 **OpenGL / Vulkan / Metal / Null 均为已实现后端**（Vulkan 1725 行 / Metal 1076 行 / Null 249 行，见 `src/rhi/`），不再是"类型预留"。条件编译：`find_package(Vulkan QUIET)` 找到 SDK 才编入 Vulkan，无 SDK 自动跳过；Metal 依赖 Apple SDK 在 macOS 上编译。
>
> **验证状态**：Vulkan 后端已于 2026-08-10 在 Windows（SDK 1.4.357）编译通过——修复了此前从未被编译暴露的 ~70 处 API 误用（swapchain/管线字段名、函数签名、缺失成员、内存屏障语义等）。当前构建 `SanYiRender_d.dll` / `SanYiCAD.exe` / `RenderxTests 15/15` 全部通过。渲染帧正确性仍需在带 HWND 的窗口下真机验证。
>
> 多窗口支持：`renderCreateDevice` 支持任意多实例，每窗口独立 `RenderDevice`，资源不共享。会话共享层 `RenderRuntime` / `RenderSession` 已落地。
>
> 已知待完善：Metal 目录尚无 `.metal` shader 源文件；Metal 后端需在 macOS 上编译验证；Vulkan 运行时渲染帧待真机验证。
>
> 性能约束：当前热路径仍存在每帧全量同步和 GPU 回读，大规模图元场景需进一步优化。

## 功能描述

SanYiRender 是一个面向 CAD 应用的专业渲染库，核心功能包括：

- **2D 渲染管线**：支持点、线、折线、多边形等基本图元的高效渲染，采用间接绘制（Indirect Draw）减少 CPU-GPU 通信开销
- **3D 网格渲染**：支持 3D 网格注册、实例化渲染、GPU 视锥剔除，适用于 CAD 模型可视化
- **叠加层渲染**：提供十字准星、捕捉指示器、预览线、选择框、选择手柄等交互 UI 元素的渲染
- **文本渲染**：基于 stb_truetype 的字体光栅化，支持世界坐标和屏幕坐标两种文本渲染模式
- **Shader 管理**：运行时从文件加载 GLSL Shader，支持 2D 场景、3D 网格、叠加层、SDF 文本、高亮等多种着色器
- **GPU 剔除**：使用 Compute Shader 实现 GPU 驱动的视锥剔除，大幅提升大规模场景的渲染性能
- **渲染图（RenderGraph）**：显式 Pass 编排与调度，支持 2D/3D 两种渲染模式的自动切换
- **管线状态缓存**：通过 PipelineStateManager 缓存和复用 RHI 管线，减少状态切换开销

## 使用方法

### 创建渲染上下文

```c
#include "render/render.h"
#include "render/render_types.h"

// 1. 准备设备描述
DeviceDesc desc;
desc.backend = BackendType::OpenGL;
desc.debugLayer = 0;
desc.nativeWindowHandle = hWnd;  // 平台相关的窗口句柄
desc.width = 1920;
desc.height = 1080;

// 2. 创建渲染设备
RenderDevice* dev = renderCreateDevice(&desc);
if (!dev) {
    // 创建失败处理
}

// 3. 设置视图模式（2D 或 3D）
renderSetViewMode(dev, ViewMode::Mode2D);

// 4. 设置清屏颜色（可选，默认浅灰色）
renderSetClearColor(dev, 0.94f, 0.94f, 0.94f, 1.0f);

// 5. 销毁设备
renderDestroyDevice(dev);
```

### 渲染流程

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

SanYiRender 采用分层架构设计，各模块职责清晰、松耦合：

```
┌─────────────────────────────────────────────────────────┐
│                    C API Facade                          │
│            (render.h / render_types.h)                  │
├─────────────────────────────────────────────────────────┤
│                    RenderDevice                          │
├──────────────┬──────────────┬───────────────────────────┤
│  RenderWorld │  RenderGraph │    CommandEncoder          │
│  (图元管理)  │  (Pass 调度) │  (统一命令编码/排序)       │
├──────────────┼──────────────┼───────────────────────────┤
│  BatchQueue  │ OverlayQueue │ PipelineStateManager       │
│  (2D 批处理) │ (叠加层)    │    (管线缓存)              │
├──────────────┼──────────────┼───────────────────────────┤
│  TextAtlas   │ MeshManager  │ PersistentEntityManager   │
│  (文本图集)  │ (3D 网格)    │    (GPU 剔除)             │
├──────────────┴──────────────┴───────────────────────────┤
│       RHI Device (OpenGL / Vulkan / Metal / Null)          │
├─────────────────────────────────────────────────────────┤
│                    Shader Manager                         │
└─────────────────────────────────────────────────────────┘
```

### C API Facade

**文件**：`include/render/render.h`、`include/render/render_types.h`

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

**文件**：`src/core/batch_queue.h` / `src/core/batch_queue.cpp`

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

**文件**：`src/shader/shaders.h` / `src/shader/shaders.cpp`

运行时从文件加载 GLSL Shader 源码，提供统一的 Shader 资源访问接口。

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

### 解决方案：相机相对渲染

采用 **每帧相机相对变换** 策略：

1. **顶点数据保持世界坐标**：不修改顶点缓冲区中的数据
2. **着色器每帧减去相机中心**：在顶点着色器中执行 `relPos = aPosition.xy - uCameraCenter`
3. **World2D 使用纯缩放矩阵**：移除视图矩阵中的平移分量，避免灾难性抵消
4. **相机中心作为 uniform 传入**：每帧更新，确保平移/缩放时精度正确

#### 着色器实现（scene_2d.vert）

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

| 场景 | 原始方案（float32） | 相机相对方案（float32） |
|------|---------------------|------------------------|
| worldX=200000, camX=199999, scale=1000 | 灾难性抵消，误差 ~24 | 减法误差 ~0.024，放大后 ~24 |
| worldX=200000, camX=199999.5, scale=1000 | 完全坍缩，误差 ~1000 | 减法误差 ~0.024，放大后 ~24 |

> **注意**：相机相对方案使用 float32 减法，在大坐标极高倍放大时仍有精度限制，但相比原始方案有数量级提升。如需更高精度，需在细分阶段使用 double 精度做减法，但需每帧重新细分。

### 新增/修改文件列表

| 文件 | 修改内容 |
|------|----------|
| `src/c_api/render_c_api_internal.h` | 添加 `double cameraCenter[2]` 到 RenderDevice |
| `include/render/render.h` | 声明 `renderSetCameraCenter()` API |
| `src/c_api/render_c_api_frame.cpp` | 实现 `renderSetCameraCenter()` |
| `src/shader/scene_2d.vert` | 添加 `uCameraCenter` uniform，相机相对变换 |
| `src/core/command_encoder.h/.cpp` | `execute()` 增加相机中心参数，World2D 使用纯缩放矩阵 |
| `src/core/scene_env.cpp` | 设置 `uCameraCenter = (0,0)` |
| `UI/.../RenderWidget.cpp` | `setViewMatrix()` 和 `initializeGL()` 中调用 `renderSetCameraCenter()` |

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

**改动文件：**

| 文件 | 修改内容 |
|------|----------|
| `Renderx/src/core/render_world_3d.h` | 新建：3D 渲染世界声明 |
| `Renderx/src/core/render_world_3d.cpp` | 新建：3D 渲染世界实现 |

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
project(SanYiRender VERSION 2.0.0 LANGUAGES CXX)

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

### Shader 文件复制

构建完成后，CMake 自动将所有 Shader 文件复制到输出目录（`$<TARGET_FILE_DIR:SanYiRender>/`），共 15 个文件：

| 类别 | 文件名 |
|------|--------|
| 2D 场景 | `scene_2d.vert`、`scene_2d.frag` |
| 叠加层 | `overlay.vert`、`overlay.frag`、`overlay_screen.vert`、`overlay_screen.frag` |
| 位图 | `bitmap.vert`、`bitmap.frag` |
| 3D 网格 | `mesh_3d.vert`、`mesh_3d.frag`、`mesh_3d_instanced.vert` |
| 文本 | `text_sdf.vert`、`text_sdf.frag`、`text_screen.vert`、`text_screen.frag` |
| 高亮 | `highlight_3d.vert`、`highlight_3d.frag` |
| GPU 剔除 | `culling.comp` |

### 字体文件复制

默认屏幕字体 `default_screen_font.ttf` 在构建后自动复制到输出目录，`renderCreateDevice` 时自动加载（14px 字号）。也可通过 `renderLoadScreenFont` 在运行时加载自定义字体。

### 测试构建

```bash
# 需要 GTest 支持
find_package(GTest QUIET)
option(BUILD_RENDERX_TESTS "Build Renderx unit tests" ON)
add_subdirectory(Test)
```

## API 概要

### C API 函数列表

#### 设备管理

| 函数 | 说明 |
|------|------|
| `renderCreateDevice` | 创建渲染设备 |
| `renderDestroyDevice` | 销毁渲染设备 |
| `renderResize` | 调整渲染目标尺寸 |
| `renderGetNativeContext` | 获取原生渲染上下文 |

#### 2D 图元管理

| 函数 | 说明 |
|------|------|
| `renderAddEntity` | 添加 2D 图元 |
| `renderModifyEntity` | 修改 2D 图元 |
| `renderRemoveEntity` | 删除 2D 图元 |
| `renderSetEntityVisibility` | 设置图元可见性 |
| `renderApplyUpdates` | 批量应用图元更新 |

#### 3D 网格管理

| 函数 | 说明 |
|------|------|
| `renderRegisterMesh` | 注册 3D 网格 |
| `renderUnregisterMesh` | 注销 3D 网格 |
| `renderAddInstance` | 添加 3D 网格实例 |
| `renderModifyInstance` | 修改 3D 网格实例 |
| `renderRemoveInstance` | 删除 3D 网格实例 |

#### 材质管理

| 函数 | 说明 |
|------|------|
| `renderAddMaterial` | 添加材质 |
| `renderUpdateMaterial` | 更新材质 |

#### 视图管理

| 函数 | 说明 |
|------|------|
| `renderSetView2D` | 设置 2D 视图参数 |
| `renderSetView3D` | 设置 3D 视图参数 |
| `renderSetViewMode` | 切换 2D/3D 视图模式 |
| `renderSetClearColor` | 设置清屏颜色 |
| `renderSetCameraCenter` | 设置相机中心（用于相机相对渲染，解决大坐标精度问题） |

#### 叠加层

| 函数 | 说明 |
|------|------|
| `renderSetOverlay` | 设置叠加层数据（十字准星、捕捉指示器） |
| `renderSubmitOverlay` | 提交单个叠加层图元 |
| `renderSubmitOverlays` | 批量提交叠加层图元 |
| `renderClearOverlays` | 清除所有叠加层图元 |
| `renderClearOverlayGroup` | 按生命周期分组清除叠加层图元 |
| `renderSetPreviewLines` | 设置预览线（已废弃，改用 `renderSubmitOverlay`） |
| `renderSetControlLines` | 设置控制线（已废弃） |
| `renderSetPointMarkers` | 设置点标记（已废弃） |
| `renderSetSelectionBox` | 设置选择框（已废弃） |
| `renderSetSelectionRect` | 设置选择预览矩形（已废弃） |
| `renderSetSelectionHandles` | 设置选择手柄（已废弃） |

#### 几何提交（统一 API）

| 函数 | 说明 |
|------|------|
| `renderSubmitGeometry` | 提交单个几何图元 |
| `renderSubmitGeometries` | 批量提交几何图元 |

> 早期分散的 `renderEmitPolyline / renderEmitCircle / renderEmitArc / renderEmitEllipse / renderEmitText / renderEmitImage / renderEmitTriangleSoup` 兼容包装器已移除，统一走 `renderSubmitGeometry`。

#### 场景环境

| 函数 | 说明 |
|------|------|
| `renderSetSceneEnv` | 设置场景环境层（网格背景、参考线） |
| `renderSetSceneEnvEx` | 设置场景环境层（扩展版，支持像素坐标和三角面） |
| `renderSetBitmap` | 设置位图图像 |
| `renderClearBitmap` | 清除位图图像 |

#### 文本渲染

| 函数 | 说明 |
|------|------|
| `renderSetTexts` | 设置文本列表（世界坐标） |
| `renderSetScreenTexts` | 设置屏幕空间文本 |
| `renderLoadScreenFont` | 加载屏幕字体 |

#### 帧渲染与统计

| 函数 | 说明 |
|------|------|
| `renderFrame` | 执行一帧渲染 |
| `renderGetStats` | 获取渲染统计信息 |
| `renderGetEntityCount` | 获取图元数量 |
| `renderGetGPUMemoryUsage` | 获取 GPU 内存使用量 |
| `renderBeginScene` | 开始场景（内部使用） |
| `renderEndScene` | 结束场景（内部使用） |

### 渲染流程说明

```
renderFrame 内部流程（2D 模式）:

┌────────────────────────────────────────────────────────────┐
│ 1. GPU 剔除阶段                                            │
│    ├─ syncWorldToPersistentManager()                       │
│    ├─ computeViewBounds()                                  │
│    ├─ executeCulling() → culling.comp                      │
│    └─ readBackGpuVisibility() 或 CPU 四叉树回退             │
├────────────────────────────────────────────────────────────┤
│ 2. BatchQueue.submit() → 构建间接绘制命令                  │
├────────────────────────────────────────────────────────────┤
│ 3. RenderGraph 执行 Pass 序列：                             │
│    ├─ Pass 0: FrameSetup        (清屏/混合/深度状态)       │
│    ├─ Pass 1: SceneEnv          (网格背景渲染)             │
│    ├─ Pass 2: World2DCollect    (2D 图元命令收集)          │
│    ├─ Pass 3: OverlayCollect    (叠加层命令收集)           │
│    ├─ Pass 4: CommandExecute    (命令排序与统一执行)       │
│    └─ Pass 5: Text              (文本渲染)                 │
├────────────────────────────────────────────────────────────┤
│ 4. 屏幕文本渲染                                             │
└────────────────────────────────────────────────────────────┘
```

### Shader 列表

| Shader 名称 | 类型 | 文件 | 用途 |
|-------------|------|------|------|
| Scene2D | 顶点+片段 | `scene_2d.vert/frag` | 2D 场景图元渲染，支持相机相对渲染（`uCameraCenter` uniform） |
| Overlay | 顶点+片段 | `overlay.vert/frag` | 叠加层渲染（世界坐标） |
| OverlayScreen | 顶点+片段 | `overlay_screen.vert/frag` | 叠加层渲染（屏幕坐标） |
| Bitmap | 顶点+片段 | `bitmap.vert/frag` | 位图图像渲染 |
| Mesh3D | 顶点+片段 | `mesh_3d.vert/frag` | 3D 网格渲染 |
| Mesh3DInstanced | 顶点 | `mesh_3d_instanced.vert` | 3D 网格实例化渲染 |
| TextSDF | 顶点+片段 | `text_sdf.vert/frag` | SDF 文本渲染 |
| TextScreen | 顶点+片段 | `text_screen.vert/frag` | 屏幕空间文本渲染 |
| Highlight3D | 顶点+片段 | `highlight_3d.vert/frag` | 3D 高亮渲染 |
| Culling | 计算 | `culling.comp` | GPU 视锥剔除 |

## 版本信息

- **当前版本**：2.0.0
- **C++ 标准**：C++17
- **构建类型**：支持 Debug（`_d` 后缀）和 Release
- **编译器支持**：MSVC、GCC、Clang