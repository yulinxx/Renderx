/**
 * @file SlotMapTests.cpp
 * @brief SlotMap 数据结构单元测试
 *
 * 测试目标：
 * - 验证 SlotMap 的插入、查找、删除操作
 * - 验证迭代器功能
 * - 验证代际管理机制
 * - 验证内存复用机制
 */

#include <gtest/gtest.h>
#include "../src/core/slot_map.h"

#include <string>

// 测试基本插入和查找
TEST(SlotMapTest, Insert_ReturnsValidKey)
{
    SlotMap<uint64_t, int> map;
    auto key = map.insert(42);

    EXPECT_NE(key, 0u);
    EXPECT_NE(map.find(key), nullptr);
    EXPECT_EQ(*map.find(key), 42);
}

TEST(SlotMapTest, Insert_MultipleItems_AllRetrievable)
{
    SlotMap<uint64_t, std::string> map;

    auto k1 = map.insert("first");
    auto k2 = map.insert("second");
    auto k3 = map.insert("third");

    EXPECT_EQ(*map.find(k1), "first");
    EXPECT_EQ(*map.find(k2), "second");
    EXPECT_EQ(*map.find(k3), "third");
    EXPECT_EQ(map.size(), 3u);
}

// 测试查找不存在的键
TEST(SlotMapTest, Find_InvalidKey_ReturnsNullptr)
{
    SlotMap<uint64_t, int> map;
    auto key = map.insert(42);

    // 使用错误的代际查找
    uint64_t wrongKey = key + (1ULL << 32);
    EXPECT_EQ(map.find(wrongKey), nullptr);
}

TEST(SlotMapTest, Find_ZeroKey_ReturnsNullptr)
{
    SlotMap<uint64_t, int> map;
    map.insert(42);

    EXPECT_EQ(map.find(0u), nullptr);
}

// 测试删除操作
TEST(SlotMapTest, Erase_RemovesElement)
{
    SlotMap<uint64_t, int> map;
    auto key = map.insert(42);

    EXPECT_EQ(map.size(), 1u);
    EXPECT_NE(map.find(key), nullptr);

    map.erase(key);

    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.find(key), nullptr);
}

TEST(SlotMapTest, Erase_InvalidatesKey)
{
    SlotMap<uint64_t, int> map;
    auto key = map.insert(42);
    map.erase(key);

    // 删除后使用旧键查找应返回 nullptr
    EXPECT_EQ(map.find(key), nullptr);
}

// 测试代际管理
TEST(SlotMapTest, Generation_IncrementedAfterErase)
{
    SlotMap<uint64_t, int> map;
    auto key1 = map.insert(42);
    map.erase(key1);

    // 重新插入应获得新一代际的键
    auto key2 = map.insert(100);

    // 旧键应无效
    EXPECT_EQ(map.find(key1), nullptr);
    // 新键应有效
    EXPECT_NE(map.find(key2), nullptr);
    EXPECT_EQ(*map.find(key2), 100);
}

// 测试内存复用
TEST(SlotMapTest, Reuse_AfterErase)
{
    SlotMap<uint64_t, int> map;

    auto k1 = map.insert(1);
    auto k2 = map.insert(2);
    auto k3 = map.insert(3);

    map.erase(k2);

    // 新插入应复用被删除的槽位
    auto k4 = map.insert(4);

    EXPECT_EQ(map.size(), 3u);
    EXPECT_NE(map.find(k1), nullptr);
    EXPECT_NE(map.find(k3), nullptr);
    EXPECT_NE(map.find(k4), nullptr);
    EXPECT_EQ(map.find(k2), nullptr);
}

// 测试迭代器
TEST(SlotMapTest, Iterator_CanIterateAllElements)
{
    SlotMap<uint64_t, int> map;
    map.insert(10);
    map.insert(20);
    map.insert(30);

    int sum = 0;
    for (const auto& val : map)
    {
        sum += val;
    }

    EXPECT_EQ(sum, 60);
}

TEST(SlotMapTest, Iterator_EmptyMap_NoIteration)
{
    SlotMap<uint64_t, int> map;

    int count = 0;
    for (const auto& val : map)
    {
        (void)val;
        count++;
    }

    EXPECT_EQ(count, 0);
}

// 测试 clear 操作
TEST(SlotMapTest, Clear_RemovesAllElements)
{
    SlotMap<uint64_t, int> map;
    auto k1 = map.insert(1);
    auto k2 = map.insert(2);
    auto k3 = map.insert(3);

    EXPECT_EQ(map.size(), 3u);

    map.clear();

    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.find(k1), nullptr);
    EXPECT_EQ(map.find(k2), nullptr);
    EXPECT_EQ(map.find(k3), nullptr);
}

// 测试 reserve 操作
TEST(SlotMapTest, Reserve_IncreasesCapacity)
{
    SlotMap<uint64_t, int> map;
    map.reserve(100);

    EXPECT_GE(map.capacity(), 100u);
}

// 测试 operator[]
TEST(SlotMapTest, SubscriptOperator_ReturnsReference)
{
    SlotMap<uint64_t, int> map;
    auto key = map.insert(42);

    EXPECT_EQ(map[key], 42);

    map[key] = 100;
    EXPECT_EQ(map[key], 100);
}

// 测试移动语义
TEST(SlotMapTest, Insert_RvalueReference)
{
    SlotMap<uint64_t, std::string> map;
    std::string value = "test";
    auto key = map.insert(std::move(value));

    EXPECT_EQ(*map.find(key), "test");
}

// 测试 EntitySlotMap 别名
TEST(SlotMapTest, EntitySlotMap_CanBeUsed)
{
    EntitySlotMap<int> map;
    auto key = map.insert(42);

    EXPECT_EQ(*map.find(key), 42);
}

// 测试边界情况
TEST(SlotMapTest, LargeNumberOfInserts)
{
    SlotMap<uint64_t, int> map;
    const int count = 1000;

    for (int i = 0; i < count; ++i)
    {
        map.insert(i);
    }

    EXPECT_EQ(map.size(), static_cast<uint32_t>(count));
}

TEST(SlotMapTest, InsertAndEraseAlternating)
{
    SlotMap<uint64_t, int> map;

    for (int i = 0; i < 100; ++i)
    {
        auto key = map.insert(i);
        if (i % 2 == 0)
        {
            map.erase(key);
        }
    }

    EXPECT_EQ(map.size(), 50u);
}
