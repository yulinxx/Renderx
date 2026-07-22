# Renderx - 高性能2D/3D渲染引擎

Renderx 是一个基于 OpenGL 的高性能渲染引擎，专为 CAD 应用设计，支持 2D 矢量图形和 3D 网格模型的渲染。

## 目录结构

```
Renderx/
├── include/
│   └── render/
│       ├── render.h        # 公共 C API 头文件
│       └── render_types.h  # 公共类型定义
├── src/
│   ├── c_api/              # C API 实现
│   │   └── render_c_api.cpp
│   ├── core/               # 核心渲染模块
│   │   ├── arena.h              # 内存竞技场分配器
│   │   ├── batch_queue.h/cpp    # 批量绘制队列
│   │   ├── mesh_manager.h/cpp   # 3D网格管理与实例化渲染
│   │   ├── overlay_queue.h/cpp  # 覆盖层渲染队列
│   │   ├── render_world.h/cpp   # 渲染世界（实体管理与空间分区）
│   │   ├── scene_env.h/cpp      # 场景环境渲染（网格背景）
│   │   ├── slot_map.h           # 插槽映射（高效实体存储）
│   │   └── text_atlas.h/cpp     # 字体图集管理（SDF文本渲染）
│   ├── rhi/                # 渲染硬件接口（RHI）
│   │   ├── rhi_device.h    # 设备接口定义
│   │   ├── rhi_gl.h/cpp    # OpenGL 实现
│   │   └── rhi_types.h     # RHI 类型定义
│   ├── shader/             # 着色器管理
│   │   ├── shaders.h/cpp   # 着色器加载与初始化
│   │   └── *.glsl          # 独立着色器文件（运行时加载）
│   └── platform/           # 平台相关代码
│       ├── gl_loader.h/cpp # OpenGL 函数加载器
│       └── stb_truetype_impl.cpp
├── Test/                   # 单元测试
├── CMakeLists.txt          # CMake 构建配置
└── README.md               # 本文件
```

## 架构设计

### 分层架构

Renderx 采用经典的分层架构设计，从底层到上层依次为：

| 层级 | 名称 | 职责 |
|------|------|------|
| **RHI层** | Render Hardware Interface | 跨平台图形硬件抽象，屏蔽不同图形API差异 |
| **核心层** | Core | 渲染逻辑核心，包括实体管理、批量绘制、空间分区等 |
| **API层** | C API | 对外暴露的 C 语言接口，便于跨语言绑定 |

### 核心模块说明

#### 0. 数据传递流程

Renderx 的数据传递从 CAD 图元到最终渲染分为以下几个阶段：

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         数据传递流程                                     │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  CAD图元数据                                                            │
│      │                                                                  │
│      ▼                                                                  │
│  ┌──────────────┐   几何细分    ┌──────────────┐                        │
│  │ 几何描述结构  │ ──────────→  │  顶点数组     │                        │
│  │ (Polyline,   │              │  (VertexP3C3) │                        │
│  │  Circle,     │              │               │                        │
│  │  Arc, etc.)  │              │  x, y, z      │                        │
│  └──────────────┘              │  r, g, b      │                        │
│                                └──────────────┘                        │
│                                      │                                  │
│                                      ▼                                  │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                     RenderWorld                                 │   │
│  │  ┌──────────┐   分配顶点空间    ┌──────────┐                    │   │
│  │  │ EntityId │ ──────────────→   │ SlotMap  │                    │   │
│  │  │ (稀疏索引)│   存储实体信息    │ (实体存储)│                    │   │
│  │  └──────────┘                  └──────────┘                    │   │
│  │         │                             │                        │   │
│  │         │ 映射实体ID                   │ 管理稠密索引            │   │
│  │         ▼                             ▼                        │   │
│  │  ┌──────────┐                  ┌─────────────┐                 │   │
│  │  │ EntityMap│                  │ VertexPool  │                 │   │
│  │  │ (ID→索引)│                  │ (顶点池)    │                 │   │
│  │  └──────────┘                  └─────────────┘                 │   │
│  │                                      │                          │   │
│  │                                      ▼                          │   │
│  │  ┌─────────────────────────────────────────────────────────┐    │   │
│  │  │                     QuadTree                            │    │   │
│  │  │  (空间分区 - 用于可见性查询)                             │    │   │
│  │  └─────────────────────────────────────────────────────────┘    │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                      │                                  │
│                                      ▼                                  │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                     BatchQueue                                  │   │
│  │  ┌────────────────┐    构建绘制命令    ┌───────────────────┐     │   │
│  │  │ 可见实体列表    │ ───────────────→  │  间接命令缓冲区     │     │   │
│  │  └────────────────┘                   │  (IndirectBuffer)  │     │   │
│  │          │                            └───────────────────┘     │   │
│  │          │ 按材质/图元分组                    │                  │   │
│  │          ▼                                   ▼                  │   │
│  │  ┌────────────────┐                   ┌───────────────────┐     │   │
│  │  │ 排序合并       │                   │  顶点缓冲区        │     │   │
│  │  │ (减少状态切换)  │                   │  (VertexBuffer)   │     │   │
│  │  └────────────────┘                   └───────────────────┘     │   │
│  │                                      │                          │   │
│  │                                      ▼                          │   │
│  │  ┌─────────────────────────────────────────────────────────┐    │   │
│  │  │              RHI Layer (OpenGL)                          │    │   │
│  │  │  ┌────────────┐   ┌────────────┐   ┌─────────────────┐   │    │   │
│  │  │  │ uploadBuffer│   │bindVertex │   │ glDrawArrays    │   │    │   │
│  │  │  │  (上传数据) │   │ Buffer    │   │ Indirect        │   │    │   │
│  │  │  └────────────┘   └────────────┘   └─────────────────┘   │    │   │
│  │  └─────────────────────────────────────────────────────────┘    │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                      │                                  │
│                                      ▼                                  │
│                           GPU 渲染输出                                  │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**数据传递详细说明**：

| 阶段 | 组件 | 数据格式 | 职责 |
|------|------|----------|------|
| 输入 | 用户代码 | 几何描述结构 (Polyline/Circle/Arc/Ellipse) | 提供CAD图元数据 |
| 细分 | tessellate* 函数 | VertexP3C3[] | 将几何描述转换为顶点数组 |
| 存储 | RenderWorld | SlotMap + VertexPool | 管理实体和顶点数据 |
| 查询 | QuadTree | 稠密索引列表 | 视锥体可见性查询 |
| 批处理 | BatchQueue | 间接命令 + VBO | 分组、排序、批量提交 |
| 渲染 | RHI层 | GPU缓冲区 | 上传数据并执行绘制 |

**2D 实体数据流程**：

1. 用户调用 `renderAddEntity()` 或 `renderEmit*()` 函数
2. C API 将几何数据细分为顶点数组 (`VertexP3C3`)
3. `RenderWorld.addEntity()` 将顶点数据存储到顶点池
4. 实体信息存储到 `SlotMap`，记录顶点偏移和数量
5. 实体包围盒插入四叉树
6. 渲染时，`BatchQueue.render()` 查询可见实体
7. 构建间接绘制命令，上传顶点数据到 GPU
8. 调用 `glDrawArraysIndirect()` 执行批量渲染

**3D 网格数据流程**：

1. 用户调用 `renderRegisterMesh()` 注册网格
2. 网格数据存储到 `MeshManager`，创建 GPU 顶点缓冲区
3. 用户调用 `renderAddInstance()` 添加实例
4. 实例数据（模型矩阵）存储到实例缓冲区
5. 渲染时，`MeshManager.render()` 使用 `glDrawElementsInstanced()` 绘制所有实例

**材质数据流程**：

1. 用户调用 `renderAddMaterial()` 创建材质
2. 材质参数（线宽、点大小、颜色）存储到 `MaterialList`
3. 渲染时，按材质索引分组实体
4. 每个材质对应的管线设置对应的 Uniform

#### 1. RenderWorld

渲染世界是实体管理的核心，负责：
- 实体的添加、修改、删除
- 顶点数据的分配和管理
- 基于四叉树的空间分区（QuadTree）
- 视锥体可见性查询

**数据结构**：
- 使用 `SlotMap` 存储实体，支持 O(1) 的插入、删除和查找
- 使用 `Arena` 内存分配器管理顶点数据，支持批量释放
- 四叉树层级固定为 4 层，最大实体数 16384

```cpp
// 创建实体
renderAddEntity(device, entityId, vertices, vertexCount, PrimitiveType::LineList, materialIdx);

// 修改实体
renderModifyEntity(device, entityId, newVertices, newVertexCount, materialIdx);

// 删除实体
renderRemoveEntity(device, entityId);
```

#### 2. BatchQueue

批量绘制队列负责将可见实体按图元类型和材质分组，使用间接绘制（`glDrawArraysIndirect`）减少 CPU-GPU 通信开销。

**核心优化**：
- 按材质和图元类型排序，减少状态切换
- 使用间接绘制命令缓冲区，批量提交绘制命令
- Dirty 范围合并，只更新修改过的区域
- 顶点缓冲区动态扩展，避免频繁重分配

**绘制流程**：
1. 提交可见实体列表
2. 按材质和图元类型分组
3. 构建间接绘制命令
4. 更新 Dirty 范围
5. 批量渲染

#### 3. OverlayQueue

覆盖层渲染队列负责绘制 UI 叠加元素，包括：
- 选择框和选择手柄
- 控制点连线
- 预览线和控制线
- 点标记
- 十字准星和捕捉点

**特性**：
- 使用世界坐标，通过视图矩阵转换到屏幕空间
- 支持多种颜色和线宽
- 合并顶点缓冲区，减少绘制调用

#### 4. MeshManager

3D 网格管理器负责：
- 网格的注册和注销
- 实例化渲染支持（一个网格多个实例）
- 实例缓冲区的管理和复用

**实例化渲染**：
- 使用 `glDrawElementsInstanced` 一次绘制多个实例
- 每个实例有独立的模型矩阵
- 支持可见性查询和遮挡剔除

```cpp
// 注册网格
MeshId mesh = renderRegisterMesh(device, positions, normals, indices, vertexCount, indexCount);

// 添加实例
uint32_t instanceId = renderAddInstance(device, mesh, modelMatrix, materialIdx);

// 修改实例
renderModifyInstance(device, instanceId, newModelMatrix);

// 删除实例
renderRemoveInstance(device, instanceId);
```

#### 5. TextAtlas

字体图集管理器使用 SDF（Signed Distance Field）技术渲染文本，支持：
- TrueType 字体加载（基于 stb_truetype）
- 动态字形缓存（按需栅格化）
- 高质量文本渲染（抗锯齿）
- 支持多种字体大小

**图集布局**：
- 图集大小：2048x2048 像素
- 行式布局，自动换行
- 字形信息缓存，避免重复栅格化

#### 6. SceneEnv

场景环境渲染负责绘制：
- 网格背景（Grid）
- 参考线
- 图层分隔线

**特性**：
- 支持多层渲染，每层可以有不同的颜色和线宽
- 使用三角形填充绘制背景区域
- 使用线绘制网格和参考线

### RHI 层设计

RHI（Render Hardware Interface）层提供了跨平台的图形硬件抽象，当前实现了 OpenGL 后端。

**设计原则**：
- 命令式 API，与传统图形 API 风格一致
- 资源管理明确（创建/销毁配对）
- 状态设置与绘制分离
- 支持间接绘制和实例化渲染
- Uniform 位置缓存，避免每帧查询

**核心接口**：
```cpp
// 资源创建
BufferHandle buffer = device->createBuffer(desc);
TextureHandle texture = device->createTexture(desc);
PipelineHandle pipeline = device->createPipeline(desc);

// 状态设置
device->bindPipeline(pipeline);
device->bindVertexBuffer(0, buffer, 0);
device->setUniformMatrix4("uViewMatrix", viewMatrix);

// 绘制命令
device->draw(vertexCount, 1, 0, 0);
device->drawIndirect(indirectBuffer, 0, drawCount, stride);
```

### Shader 系统

Shader 系统采用运行时加载方式，从独立的 `.glsl` 文件读取源码。

**支持的 Shader**：

| Shader 文件 | 用途 |
|------------|------|
| `scene_2d.vert/frag` | 2D 场景渲染 |
| `overlay.vert/frag` | 世界坐标叠加层渲染 |
| `overlay_screen.vert/frag` | 屏幕坐标叠加层渲染 |
| `bitmap.vert/frag` | 位图渲染 |
| `mesh_3d.vert/frag` | 3D 网格渲染 |
| `mesh_3d_instanced.vert` | 3D 网格实例化渲染 |
| `text_sdf.vert/frag` | SDF 文本渲染 |
| `highlight_3d.vert/frag` | 3D 高亮渲染 |

## 使用方法

### 基本流程

1. **创建设备**
```cpp
render::DeviceDesc desc;
desc.backend = render::BackendType::OpenGL;
desc.nativeWindowHandle = nativeWindow;
desc.width = width;
desc.height = height;

render::RenderDevice* device = renderCreateDevice(&desc);
```

2. **设置视图**
```cpp
// 2D 视图
float viewMatrix[9] = { ... };
renderSetView2D(device, viewMatrix, viewWidth, viewHeight);

// 3D 视图
float viewMatrix[16] = { ... };
float projMatrix[16] = { ... };
renderSetView3D(device, viewMatrix, projMatrix);
```

3. **添加实体**
```cpp
render::VertexP3C3 vertices[] = {
    { 0, 0, 0, 1, 0, 0 },
    { 1, 0, 0, 0, 1, 0 },
    { 0.5, 1, 0, 0, 0, 1 },
};

renderAddEntity(device, entityId, vertices, 3, 
                render::PrimitiveType::TriangleList, materialIdx);
```

4. **渲染帧**
```cpp
renderFrame(device);
```

5. **销毁设备**
```cpp
renderDestroyDevice(device);
```

### 材质管理

```cpp
render::MaterialDesc material;
material.lineWidth = 1.0f;
material.pointSize = 2.0f;
material.color[0] = 1.0f; // R
material.color[1] = 0.5f; // G
material.color[2] = 0.0f; // B
material.color[3] = 1.0f; // A

uint16_t materialIdx = renderAddMaterial(device, &material);
```

### 覆盖层设置

```cpp
// 设置预览线
renderSetPreviewLines(device, vertices, vertexCount, 0xFFFF00FF);

// 设置选择框
render::BBox2f bbox = { 0, 0, 100, 100 };
renderSetSelectionBox(device, &bbox, 0xFF00FF00);

// 设置文本
render::TextItem items[] = {
    { "Hello", 100, 200, 0, 1, 1, 16, { 1, 1, 1, 1 }, 0, 0 },
};
render::TextItemList textList = { items, 1 };
renderSetTexts(device, &textList);
```

### 场景模式

场景模式支持从几何数据直接发射实体，无需手动管理实体ID：

```cpp
// 开始场景（清除旧实体）
renderBeginScene(device);

// 发射几何
renderEmitPolyline(device, &polyline);
renderEmitCircle(device, &circle);
renderEmitArc(device, &arc);
renderEmitEllipse(device, &ellipse);

// 渲染帧（会自动包含场景中的实体）
renderFrame(device);
```

## 构建说明

### 依赖

- CMake 3.16+
- OpenGL 3.3+
- Visual Studio 2019+（Windows）或 GCC 8+（Linux）
- C++17 标准库（std::filesystem）
- stb_truetype（可选，用于文本渲染）

### 构建步骤

```bash
# 创建构建目录
mkdir build && cd build

# 配置 CMake（Windows）
cmake .. -G "Visual Studio 17 2022" -A x64

# 配置 CMake（Linux）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build . --config Release
```

### 输出

- **动态库**：`bin_Qt6/Release/Renderx.dll`（Windows）或 `libRenderx.so`（Linux）
- **头文件**：`include/render/render.h` 和 `include/render/render_types.h`
- **Shader 文件**：自动复制到输出目录

## 设计理念

### 性能优先

- **批量绘制**：使用间接绘制减少绘制调用
- **空间分区**：四叉树实现高效可见性查询
- **Dirty 范围合并**：只更新修改过的数据
- **缓冲区复用**：避免频繁的分配和销毁
- **Uniform 缓存**：避免每帧查询 Uniform 位置

### 可扩展性

- **RHI 抽象**：易于添加新的图形后端（Vulkan、Metal）
- **模块化设计**：各模块职责清晰，易于扩展
- **C API 封装**：便于跨语言绑定（Python、C#等）
- **运行时 Shader 加载**：便于调试和热更新

### 易用性

- **C 语言接口**：简单易用，无需 C++ 知识
- **统一的资源管理**：创建/销毁配对，避免资源泄漏
- **完善的文档**：所有 API 都有详细的 Doxygen 风格注释

## 性能优化策略

1. **批量渲染**：使用 `glDrawArraysIndirect` 一次提交多个绘制命令
2. **空间分区**：四叉树实现 O(log n) 可见性查询
3. **Dirty 范围合并**：将相邻的 Dirty 范围合并，减少 GPU 数据上传
4. **缓冲区预分配**：顶点缓冲区和实例缓冲区预分配，避免频繁重分配
5. **Uniform 缓存**：在管线创建时缓存 Uniform 位置，避免每帧查询
6. **状态排序**：按材质和图元类型排序，减少状态切换

## 未来规划

- [ ] Vulkan 后端支持
- [ ] Metal 后端支持（Apple 平台）
- [ ] 纹理加载和管理
- [ ] 高级光照系统
- [ ] 渲染管线编辑器
- [ ] 更多字体格式支持（OTF）
- [ ] 离屏渲染支持
- [ ] 阴影渲染

## 许可证

MIT License
