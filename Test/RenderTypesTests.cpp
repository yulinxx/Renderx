/**
 * @file RenderTypesTests.cpp
 * @brief 渲染核心类型单元测试
 *
 * 测试目标：
 * - 验证 RenderTypes.h 中定义的核心数据结构大小和布局
 * - 验证枚举类型的值定义
 * - 验证常量定义的正确性
 */

#include <gtest/gtest.h>
#include "render/RenderTypes.h"

// 测试顶点结构体大小
TEST(RenderTypesTest, VertexP3C3_SizeIs24Bytes)
{
    EXPECT_EQ(sizeof(render::VertexP3C3), 24u);
}

TEST(RenderTypesTest, VertexP3N3_SizeIs24Bytes)
{
    EXPECT_EQ(sizeof(render::VertexP3N3), 24u);
}

// 测试无效 ID 常量
TEST(RenderTypesTest, InvalidEntityId_IsZero)
{
    EXPECT_EQ(render::INVALID_ENTITY_ID, 0u);
}

TEST(RenderTypesTest, InvalidMeshId_IsZero)
{
    EXPECT_EQ(render::INVALID_MESH_ID, 0u);
}

// 测试图元类型枚举
TEST(RenderTypesTest, PrimitiveType_ValuesAreCorrect)
{
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::PointList), 0);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::LineList), 1);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::LineStrip), 2);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::LineLoop), 3);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::TriangleList), 4);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::TriangleStrip), 5);
    EXPECT_EQ(static_cast<uint8_t>(render::PrimitiveType::TriangleFan), 6);
}

TEST(RenderTypesTest, PrimitiveType_CountIs7)
{
    EXPECT_EQ(render::PRIMITIVE_TYPE_COUNT, 7u);
}

// 测试更新操作枚举
TEST(RenderTypesTest, UpdateOp_ValuesAreCorrect)
{
    EXPECT_EQ(static_cast<uint8_t>(render::UpdateOp::Add), 0);
    EXPECT_EQ(static_cast<uint8_t>(render::UpdateOp::Modify), 1);
    EXPECT_EQ(static_cast<uint8_t>(render::UpdateOp::Remove), 2);
}

// 测试图元标志枚举
TEST(RenderTypesTest, EntityFlags_ValuesAreCorrect)
{
    EXPECT_EQ(static_cast<uint32_t>(render::EntityFlags::None), 0);
    EXPECT_EQ(static_cast<uint32_t>(render::EntityFlags::Visible), 1);
    EXPECT_EQ(static_cast<uint32_t>(render::EntityFlags::Selected), 2);
    EXPECT_EQ(static_cast<uint32_t>(render::EntityFlags::Highlighted), 4);
}

TEST(RenderTypesTest, EntityFlags_CanBeCombined)
{
    auto flags = render::EntityFlags::Visible | render::EntityFlags::Selected;
    EXPECT_EQ(static_cast<uint32_t>(flags), 3);
}

// 测试后端类型枚举
TEST(RenderTypesTest, BackendType_ValuesAreCorrect)
{
    EXPECT_EQ(static_cast<int>(render::BackendType::OpenGL), 0);
    EXPECT_EQ(static_cast<int>(render::BackendType::Vulkan), 1);
    EXPECT_EQ(static_cast<int>(render::BackendType::Metal), 2);
    EXPECT_EQ(static_cast<int>(render::BackendType::Null), 3);
}

// 测试结构体默认值
TEST(RenderTypesTest, EntityUpdate_DefaultConstruction)
{
    render::EntityUpdate update{};
    EXPECT_EQ(update.entityId, 0u);
    EXPECT_EQ(update.vertexCount, 0u);
    EXPECT_EQ(update.primitiveType, 0);
    EXPECT_EQ(update.materialIndex, 0);
}

TEST(RenderTypesTest, MaterialDesc_DefaultConstruction)
{
    render::MaterialDesc desc{};
    EXPECT_FLOAT_EQ(desc.lineWidth, 0.0f);
    EXPECT_FLOAT_EQ(desc.pointSize, 0.0f);
    EXPECT_EQ(desc.flags, 0u);
}

TEST(RenderTypesTest, EntityDesc_DefaultConstruction)
{
    render::EntityDesc desc{};
    EXPECT_EQ(desc.entityId, 0u);
    EXPECT_EQ(desc.vertexOffset, 0u);
    EXPECT_EQ(desc.vertexCount, 0u);
    EXPECT_EQ(desc.primitiveType, 0);
    EXPECT_EQ(desc.materialIndex, 0);
    EXPECT_EQ(desc.flags, 0u);
}

TEST(RenderTypesTest, BBox2f_DefaultConstruction)
{
    render::BBox2f bbox{};
    EXPECT_FLOAT_EQ(bbox.minX, 0.0f);
    EXPECT_FLOAT_EQ(bbox.minY, 0.0f);
    EXPECT_FLOAT_EQ(bbox.maxX, 0.0f);
    EXPECT_FLOAT_EQ(bbox.maxY, 0.0f);
}

// 测试结构体赋值
TEST(RenderTypesTest, VertexP3C3_CanBeAssigned)
{
    render::VertexP3C3 v{};
    v.px = 1.0f;
    v.py = 2.0f;
    v.pz = 3.0f;
    v.cr = 1.0f;
    v.cg = 0.5f;
    v.cb = 0.0f;

    EXPECT_FLOAT_EQ(v.px, 1.0f);
    EXPECT_FLOAT_EQ(v.py, 2.0f);
    EXPECT_FLOAT_EQ(v.pz, 3.0f);
    EXPECT_FLOAT_EQ(v.cr, 1.0f);
    EXPECT_FLOAT_EQ(v.cg, 0.5f);
    EXPECT_FLOAT_EQ(v.cb, 0.0f);
}

TEST(RenderTypesTest, ViewDesc2D_CanBeAssigned)
{
    render::ViewDesc2D view{};
    view.viewWidth = 800.0f;
    view.viewHeight = 600.0f;

    EXPECT_FLOAT_EQ(view.viewWidth, 800.0f);
    EXPECT_FLOAT_EQ(view.viewHeight, 600.0f);
}
