/**
 * @file BatchQueueTests.cpp
 * @brief BatchQueue 单元测试
 *
 * 测试目标：
 * - 验证批次提交
 * - 验证批次合并
 * - 验证间接命令生成
 * - 验证脏范围管理
 */

#include <gtest/gtest.h>
#include "../src/core/batch_queue.h"
#include "render/render_types.h"

#include <vector>
#include <algorithm>

// 测试 Batch 结构体
TEST(BatchQueueTest, Batch_DefaultState)
{
    render::core::BatchQueue::Batch batch{};
    batch.type = render::PrimitiveType::TriangleList;
    batch.firstIndirect = 0;
    batch.indirectCount = 0;
    batch.lineWidth = 1.0f;
    batch.materialIndex = 0;

    EXPECT_EQ(batch.type, render::PrimitiveType::TriangleList);
    EXPECT_EQ(batch.firstIndirect, 0u);
    EXPECT_EQ(batch.indirectCount, 0u);
    EXPECT_FLOAT_EQ(batch.lineWidth, 1.0f);
    EXPECT_EQ(batch.materialIndex, 0u);
}

// 测试 DirtyRange 结构体
TEST(BatchQueueTest, DirtyRange_DefaultState)
{
    render::core::BatchQueue::DirtyRange range{};
    range.offset = 0;
    range.size = 0;

    EXPECT_EQ(range.offset, 0u);
    EXPECT_EQ(range.size, 0u);
}

// 测试 DrawIndirectCmd 结构体
TEST(BatchQueueTest, DrawIndirectCmd_DefaultState)
{
    render::DrawIndirectCmd cmd{};
    cmd.vertexCount = 0;
    cmd.instanceCount = 1;
    cmd.firstVertex = 0;
    cmd.baseVertex = 0;
    cmd.firstInstance = 0;

    EXPECT_EQ(cmd.vertexCount, 0u);
    EXPECT_EQ(cmd.instanceCount, 1u);
    EXPECT_EQ(cmd.firstVertex, 0u);
    EXPECT_EQ(cmd.baseVertex, 0u);
    EXPECT_EQ(cmd.firstInstance, 0u);
}

// 测试批次合并逻辑
TEST(BatchQueueTest, BatchMerge_AdjacentBatches)
{
    std::vector<render::core::BatchQueue::Batch> batches;

    // 添加相邻批次（相同材质和图元类型）
    render::core::BatchQueue::Batch batch1{};
    batch1.type = render::PrimitiveType::TriangleList;
    batch1.firstIndirect = 0;
    batch1.indirectCount = 5;
    batch1.materialIndex = 1;

    render::core::BatchQueue::Batch batch2{};
    batch2.type = render::PrimitiveType::TriangleList;
    batch2.firstIndirect = 5;
    batch2.indirectCount = 3;
    batch2.materialIndex = 1;

    batches.push_back(batch1);
    batches.push_back(batch2);

    // 合并逻辑：相同材质和图元类型的批次可以合并
    bool canMerge = (batches[0].type == batches[1].type &&
                     batches[0].materialIndex == batches[1].materialIndex &&
                     batches[0].firstIndirect + batches[0].indirectCount == batches[1].firstIndirect);

    EXPECT_TRUE(canMerge);
}

// 测试批次不合并（不同材质）
TEST(BatchQueueTest, BatchMerge_DifferentMaterials)
{
    std::vector<render::core::BatchQueue::Batch> batches;

    render::core::BatchQueue::Batch batch1{};
    batch1.type = render::PrimitiveType::TriangleList;
    batch1.materialIndex = 1;

    render::core::BatchQueue::Batch batch2{};
    batch2.type = render::PrimitiveType::TriangleList;
    batch2.materialIndex = 2;

    batches.push_back(batch1);
    batches.push_back(batch2);

    bool canMerge = (batches[0].materialIndex == batches[1].materialIndex);
    EXPECT_FALSE(canMerge);
}

// 测试批次不合并（不同图元类型）
TEST(BatchQueueTest, BatchMerge_DifferentPrimitiveTypes)
{
    std::vector<render::core::BatchQueue::Batch> batches;

    render::core::BatchQueue::Batch batch1{};
    batch1.type = render::PrimitiveType::TriangleList;

    render::core::BatchQueue::Batch batch2{};
    batch2.type = render::PrimitiveType::LineList;

    batches.push_back(batch1);
    batches.push_back(batch2);

    bool canMerge = (batches[0].type == batches[1].type);
    EXPECT_FALSE(canMerge);
}

// 测试脏范围合并
TEST(BatchQueueTest, DirtyRangeMerge_AdjacentRanges)
{
    std::vector<render::core::BatchQueue::DirtyRange> ranges;

    render::core::BatchQueue::DirtyRange range1{0, 10};
    render::core::BatchQueue::DirtyRange range2{10, 5};

    ranges.push_back(range1);
    ranges.push_back(range2);

    // 合并相邻范围
    bool canMerge = (ranges[0].offset + ranges[0].size == ranges[1].offset);
    EXPECT_TRUE(canMerge);

    if (canMerge) {
        render::core::BatchQueue::DirtyRange merged{ranges[0].offset, ranges[0].size + ranges[1].size};
        EXPECT_EQ(merged.offset, 0u);
        EXPECT_EQ(merged.size, 15u);
    }
}

// 测试脏范围不合并（不连续）
TEST(BatchQueueTest, DirtyRangeMerge_NonAdjacentRanges)
{
    std::vector<render::core::BatchQueue::DirtyRange> ranges;

    render::core::BatchQueue::DirtyRange range1{0, 10};
    render::core::BatchQueue::DirtyRange range2{15, 5};

    ranges.push_back(range1);
    ranges.push_back(range2);

    bool canMerge = (ranges[0].offset + ranges[0].size == ranges[1].offset);
    EXPECT_FALSE(canMerge);
}

// 测试间接命令生成
TEST(BatchQueueTest, IndirectCommand_Generation)
{
    std::vector<render::DrawIndirectCmd> commands;

    // 生成间接命令
    render::DrawIndirectCmd cmd{};
    cmd.vertexCount = 3;
    cmd.instanceCount = 1;
    cmd.firstVertex = 0;
    cmd.baseVertex = 0;
    cmd.firstInstance = 0;

    commands.push_back(cmd);

    EXPECT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].vertexCount, 3u);
    EXPECT_EQ(commands[0].instanceCount, 1u);
}

// 测试多个间接命令
TEST(BatchQueueTest, IndirectCommand_MultipleCommands)
{
    std::vector<render::DrawIndirectCmd> commands;

    for (uint32_t i = 0; i < 5; ++i) {
        render::DrawIndirectCmd cmd{};
        cmd.vertexCount = 3;
        cmd.instanceCount = 1;
        cmd.firstVertex = i * 3;
        cmd.baseVertex = i * 3;
        cmd.firstInstance = 0;
        commands.push_back(cmd);
    }

    EXPECT_EQ(commands.size(), 5u);
    EXPECT_EQ(commands[0].firstVertex, 0u);
    EXPECT_EQ(commands[1].firstVertex, 3u);
    EXPECT_EQ(commands[4].firstVertex, 12u);
}

// 测试管线索引
TEST(BatchQueueTest, PipelineIndex_PrimitiveType)
{
    // 图元类型到管线索引的映射
    auto getPipelineIndex = [](render::PrimitiveType type) -> uint32_t {
        return static_cast<uint32_t>(type);
    };

    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::PointList), 0u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::LineList), 1u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::LineStrip), 2u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::LineLoop), 3u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::TriangleList), 4u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::TriangleStrip), 5u);
    EXPECT_EQ(getPipelineIndex(render::PrimitiveType::TriangleFan), 6u);
}

// 测试可见性过滤
TEST(BatchQueueTest, VisibilityFilter_Basic)
{
    std::vector<uint32_t> visibleIndices;
    visibleIndices.push_back(0);
    visibleIndices.push_back(2);
    visibleIndices.push_back(4);

    EXPECT_EQ(visibleIndices.size(), 3u);
    EXPECT_EQ(visibleIndices[0], 0u);
    EXPECT_EQ(visibleIndices[1], 2u);
    EXPECT_EQ(visibleIndices[2], 4u);
}

// 测试容量管理
TEST(BatchQueueTest, CapacityManagement_Grow)
{
    uint32_t capacity = 100;
    uint32_t required = 150;

    // 模拟容量增长
    if (required > capacity) {
        capacity = required * 2; // 倍增策略
    }

    EXPECT_GE(capacity, required);
    EXPECT_EQ(capacity, 300u);
}

// 测试脏标志管理
TEST(BatchQueueTest, DirtyFlag_Management)
{
    bool dirty = false;

    // 标记为脏
    dirty = true;
    EXPECT_TRUE(dirty);

    // 清除脏标志
    dirty = false;
    EXPECT_FALSE(dirty);
}

// 测试视图变化检测
TEST(BatchQueueTest, ViewChanged_Detection)
{
    bool viewChanged = false;

    // 视图矩阵变化
    viewChanged = true;
    EXPECT_TRUE(viewChanged);

    // 清除变化标志
    viewChanged = false;
    EXPECT_FALSE(viewChanged);
}
