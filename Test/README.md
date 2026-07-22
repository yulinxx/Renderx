# Renderx 测试

渲染引擎单元测试。

## 测试文件

| 文件 | 测试内容 |
|------|----------|
| RenderTypesTests.cpp | 核心类型、枚举、结构体大小验证 |
| SlotMapTests.cpp | SlotMap 数据结构（插入/删除/代际管理/内存复用） |
| ArenaTests.cpp | Arena 内存分配器（分配/对齐/重置/容量） |
| MeshManagerTests.cpp | 网格管理器（注册/注销/实例管理/可见性查询） |
| BatchQueueTests.cpp | 批次队列（提交/合并/间接命令生成/脏范围） |

## 运行测试

```bash
# 编译
cmake --build build --target RenderxTests

# 运行
./build/bin_Qt6/Release/RenderxTests
```
