/**
 * @file ArenaTests.cpp
 * @brief Arena 内存分配器单元测试
 *
 * 测试目标：
 * - 验证 Arena 的基本分配功能
 * - 验证对齐机制
 * - 验证重置功能
 * - 验证容量管理
 */

#include <gtest/gtest.h>
#include "../src/core/arena.h"

#include <string>

// 测试基本构造
TEST(ArenaTest, DefaultConstruction_HasCapacity)
{
    Arena arena;
    EXPECT_GT(arena.capacity(), 0u);
    EXPECT_EQ(arena.used(), 0u);
}

TEST(ArenaTest, CustomBlockSize_RespectsSize)
{
    Arena arena(1024);
    EXPECT_EQ(arena.capacity(), 1024u);
    EXPECT_EQ(arena.used(), 0u);
}

// 测试基本分配
TEST(ArenaTest, Allocate_ReturnsValidPointer)
{
    Arena arena(1024);
    void* ptr = arena.allocate(100);

    EXPECT_NE(ptr, nullptr);
    EXPECT_GE(arena.used(), 100u);
}

TEST(ArenaTest, Allocate_MultipleAllocations_AdvanceOffset)
{
    Arena arena(1024);

    void* p1 = arena.allocate(100);
    size_t used1 = arena.used();

    void* p2 = arena.allocate(100);
    size_t used2 = arena.used();

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_GT(used2, used1);
}

// 测试对齐
TEST(ArenaTest, Allocate_RespectsAlignment)
{
    Arena arena(1024);

    void* p1 = arena.allocate(1, 1);
    void* p2 = arena.allocate(8, 8);
    void* p4 = arena.allocate(16, 16);

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p4, nullptr);

    // 检查对齐
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 8, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p4) % 16, 0u);
}

// 测试分配失败
TEST(ArenaTest, Allocate_ExceedsCapacity_ReturnsNullptr)
{
    Arena arena(100);

    void* ptr = arena.allocate(200);
    EXPECT_EQ(ptr, nullptr);
}

TEST(ArenaTest, Allocate_Fragmentation_ReturnsNullptr)
{
    Arena arena(100);

    // 分配小块直到空间不足
    void* p1 = arena.allocate(40);
    void* p2 = arena.allocate(40);
    void* p3 = arena.allocate(40); // 应该失败

    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_EQ(p3, nullptr);
}

// 测试 emplace
TEST(ArenaTest, Emplace_ConstructsObject)
{
    Arena arena(1024);

    int* ptr = arena.emplace<int>(42);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, 42);
}

TEST(ArenaTest, Emplace_ComplexType)
{
    Arena arena(1024);

    std::string* ptr = arena.emplace<std::string>("hello");
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, "hello");
}

TEST(ArenaTest, Emplace_ExceedsCapacity_ReturnsNullptr)
{
    Arena arena(10);

    std::string* ptr = arena.emplace<std::string>("this is a very long string that exceeds capacity");
    EXPECT_EQ(ptr, nullptr);
}

// 测试重置
TEST(ArenaTest, Reset_ClearsUsedSpace)
{
    Arena arena(1024);

    arena.allocate(100);
    arena.allocate(200);
    EXPECT_GT(arena.used(), 0u);

    arena.reset();
    EXPECT_EQ(arena.used(), 0u);
}

TEST(ArenaTest, Reset_AllowsReallocation)
{
    Arena arena(100);

    void* p1 = arena.allocate(80);
    EXPECT_NE(p1, nullptr);

    void* p2 = arena.allocate(80); // 应该失败
    EXPECT_EQ(p2, nullptr);

    arena.reset();

    void* p3 = arena.allocate(80); // 重置后应该成功
    EXPECT_NE(p3, nullptr);
}

// 测试容量查询
TEST(ArenaTest, Capacity_RemainsConstant)
{
    Arena arena(1024);
    size_t initialCapacity = arena.capacity();

    arena.allocate(100);
    arena.allocate(200);

    EXPECT_EQ(arena.capacity(), initialCapacity);
}

TEST(ArenaTest, Used_TracksAllocation)
{
    Arena arena(1024);

    EXPECT_EQ(arena.used(), 0u);

    arena.allocate(100);
    size_t used1 = arena.used();
    EXPECT_GE(used1, 100u);

    arena.allocate(100);
    size_t used2 = arena.used();
    EXPECT_GT(used2, used1);
}

// 测试边界情况
TEST(ArenaTest, Allocate_ZeroSize)
{
    Arena arena(1024);
    void* ptr = arena.allocate(0);

    // 零大小分配应返回有效指针或不改变状态
    EXPECT_EQ(arena.used(), 0u);
}

TEST(ArenaTest, Allocate_ExactCapacity)
{
    Arena arena(100);
    void* ptr = arena.allocate(100);

    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(arena.used(), 100u);
}

// 测试连续分配和重置循环
TEST(ArenaTest, MultipleResetCycles)
{
    Arena arena(1024);

    for (int i = 0; i < 10; ++i) {
        void* ptr = arena.allocate(100);
        EXPECT_NE(ptr, nullptr);
        arena.reset();
        EXPECT_EQ(arena.used(), 0u);
    }
}
