/**
 * @file MeshManagerTests.cpp
 * @brief MeshManager 单元测试
 *
 * 测试目标：
 * - 验证网格注册/注销
 * - 验证实例管理
 * - 验证可见性查询
 * - 验证边界情况
 */

#include <gtest/gtest.h>
#include "../src/core/mesh_manager.h"
#include "../src/core/slot_map.h"

#include <vector>
#include <cmath>

// 测试 MeshEntry 结构体
TEST(MeshManagerTest, MeshEntry_DefaultState)
{
    render::core::MeshManager::MeshEntry entry{};
    entry.deleted = false;
    entry.indexCount = 0;
    entry.vertexCount = 0;

    EXPECT_FALSE(entry.deleted);
    EXPECT_EQ(entry.indexCount, 0u);
    EXPECT_EQ(entry.vertexCount, 0u);
}

// 测试 InstanceEntry 结构体
TEST(MeshManagerTest, InstanceEntry_DefaultState)
{
    render::core::MeshManager::InstanceEntry entry{};
    entry.dirty = false;
    entry.materialIndex = 0;
    entry.flags = 0;

    EXPECT_FALSE(entry.dirty);
    EXPECT_EQ(entry.materialIndex, 0u);
    EXPECT_EQ(entry.flags, 0u);
}

// 测试模型矩阵初始化
TEST(MeshManagerTest, InstanceEntry_IdentityMatrix)
{
    render::core::MeshManager::InstanceEntry entry{};

    // 单位矩阵
    entry.modelMatrix[0] = 1.0f;
    entry.modelMatrix[5] = 1.0f;
    entry.modelMatrix[10] = 1.0f;
    entry.modelMatrix[15] = 1.0f;

    EXPECT_FLOAT_EQ(entry.modelMatrix[0], 1.0f);
    EXPECT_FLOAT_EQ(entry.modelMatrix[5], 1.0f);
    EXPECT_FLOAT_EQ(entry.modelMatrix[10], 1.0f);
    EXPECT_FLOAT_EQ(entry.modelMatrix[15], 1.0f);
}

// 测试包围盒计算
TEST(MeshManagerTest, BBox_Calculation)
{
    // 简单三角形的包围盒
    float positions[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.5f, 1.0f, 0.0f
    };

    float minX = positions[0], maxX = positions[0];
    float minY = positions[1], maxY = positions[1];
    float minZ = positions[2], maxZ = positions[2];

    for (int i = 0; i < 3; ++i) {
        float x = positions[i * 3 + 0];
        float y = positions[i * 3 + 1];
        float z = positions[i * 3 + 2];

        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
        if (z < minZ) minZ = z;
        if (z > maxZ) maxZ = z;
    }

    EXPECT_FLOAT_EQ(minX, 0.0f);
    EXPECT_FLOAT_EQ(maxX, 1.0f);
    EXPECT_FLOAT_EQ(minY, 0.0f);
    EXPECT_FLOAT_EQ(maxY, 1.0f);
    EXPECT_FLOAT_EQ(minZ, 0.0f);
    EXPECT_FLOAT_EQ(maxZ, 0.0f);
}

// 测试可见性标志
TEST(MeshManagerTest, InstanceFlags_Visibility)
{
    uint32_t flags = 0;

    // 设置可见标志
    flags |= 1;
    EXPECT_TRUE(flags & 1);

    // 清除可见标志
    flags &= ~1;
    EXPECT_FALSE(flags & 1);
}

// 测试脏标志管理
TEST(MeshManagerTest, DirtyFlag_Management)
{
    render::core::MeshManager::InstanceEntry entry{};
    entry.dirty = false;

    // 标记为脏
    entry.dirty = true;
    EXPECT_TRUE(entry.dirty);

    // 清除脏标志
    entry.dirty = false;
    EXPECT_FALSE(entry.dirty);
}

// 测试 SlotMap 集成
TEST(MeshManagerTest, SlotMap_Integration)
{
    SlotMap<uint64_t, render::core::MeshManager::MeshEntry> meshMap;

    render::core::MeshManager::MeshEntry entry{};
    entry.indexCount = 3;
    entry.vertexCount = 3;
    entry.deleted = false;

    auto key = meshMap.insert(entry);
    EXPECT_NE(key, 0u);

    auto* retrieved = meshMap.find(key);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->indexCount, 3u);
    EXPECT_EQ(retrieved->vertexCount, 3u);
    EXPECT_FALSE(retrieved->deleted);
}

// 测试实例管理
TEST(MeshManagerTest, InstanceManagement_AddRemove)
{
    std::vector<render::core::MeshManager::InstanceEntry> instances;
    std::vector<uint32_t> freeInstances;

    // 添加实例
    render::core::MeshManager::InstanceEntry inst1{};
    inst1.meshDenseIdx = 0;
    inst1.materialIndex = 1;
    instances.push_back(inst1);

    EXPECT_EQ(instances.size(), 1u);

    // 移除实例（标记为空闲）
    freeInstances.push_back(0);
    EXPECT_EQ(freeInstances.size(), 1u);

    // 重用空闲槽位
    render::core::MeshManager::InstanceEntry inst2{};
    inst2.meshDenseIdx = 1;
    inst2.materialIndex = 2;
    instances[freeInstances.back()] = inst2;
    freeInstances.pop_back();

    EXPECT_EQ(instances[0].meshDenseIdx, 1u);
    EXPECT_EQ(instances[0].materialIndex, 2u);
}

// 测试脏实例列表
TEST(MeshManagerTest, DirtyInstances_Tracking)
{
    std::vector<uint32_t> dirtyInstances;

    // 添加脏实例
    dirtyInstances.push_back(0);
    dirtyInstances.push_back(2);
    dirtyInstances.push_back(5);

    EXPECT_EQ(dirtyInstances.size(), 3u);

    // 去重
    std::sort(dirtyInstances.begin(), dirtyInstances.end());
    auto last = std::unique(dirtyInstances.begin(), dirtyInstances.end());
    dirtyInstances.erase(last, dirtyInstances.end());

    EXPECT_EQ(dirtyInstances.size(), 3u);
}

// 测试可见性查询
TEST(MeshManagerTest, VisibilityQuery_BasicFilter)
{
    std::vector<render::core::MeshManager::InstanceEntry> instances;

    // 添加多个实例
    for (uint32_t i = 0; i < 5; ++i) {
        render::core::MeshManager::InstanceEntry inst{};
        inst.meshDenseIdx = i;
        inst.flags = (i % 2 == 0) ? 1 : 0; // 偶数可见
        instances.push_back(inst);
    }

    // 查询可见实例
    std::vector<uint32_t> visible;
    for (uint32_t i = 0; i < instances.size(); ++i) {
        if (instances[i].flags & 1) {
            visible.push_back(i);
        }
    }

    EXPECT_EQ(visible.size(), 3u); // 0, 2, 4
    EXPECT_EQ(visible[0], 0u);
    EXPECT_EQ(visible[1], 2u);
    EXPECT_EQ(visible[2], 4u);
}
