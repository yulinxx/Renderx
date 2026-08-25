/**
 * @file RuntimeSessionTests.cpp
 * @brief Tests for the new decoupled Render::RT runtime/session API (Null backend).
 *
 * Validates the "renderer only knows how to draw bytes" contract:
 *  - Runtime (shared GPU resources) creation + default pipelines
 *  - Persistent buffer lifecycle
 *  - Transient single-frame allocation bookkeeping
 *  - Session per-window submit + draw counting + stats
 *  - Sort key bit layout
 *  - Visibility (AABB) culling
 *  - Material add/update
 *
 * No GPU/OpenGL context required (Null backend).
 */
#include <gtest/gtest.h>

#include "render/runtime_session.h"

using namespace Render::RT;

namespace
{

    RuntimeDesc makeNullDesc()
    {
        RuntimeDesc d{};
        d.backend = Backend::Null;
        d.transientBufferSize = 1024;
        return d;
    }

    SessionDesc makeSessionDesc(RuntimeHandle rt, uint32_t w, uint32_t h)
    {
        SessionDesc d{};
        d.runtime = rt;
        d.width = w;
        d.height = h;
        d.clearColor[0] = 0.1f;
        d.clearColor[1] = 0.2f;
        d.clearColor[2] = 0.3f;
        d.clearColor[3] = 1.0f;
        return d;
    }

}  // namespace

TEST(RTSessionTest, RuntimeCreateNullAndDefaults)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    for (int i = 0; i < static_cast<int>(DefaultPipeline::Count); ++i)
    {
        uint16_t p = runtimeGetDefaultPipeline(rt, static_cast<DefaultPipeline>(i));
        EXPECT_NE(p, 0u) << "default pipeline " << i << " must be valid";
    }

    runtimeDestroy(rt);
}

TEST(RTSessionTest, BufferLifecycle)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    RTBufferDesc bd{};
    bd.size = 1024;
    bd.usageFlags = 1;  // vertex
    BufferHandle buf = runtimeCreateBuffer(rt, &bd);
    EXPECT_NE(buf, 0u);

    float data[4] = { 1, 2, 3, 4 };
    runtimeUploadBuffer(rt, buf, 0, sizeof(data), data);  // must not crash

    runtimeDestroyBuffer(rt, buf);

    runtimeDestroy(rt);
}

TEST(RTSessionTest, TransientAllocation)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    runtimeFrameBegin(rt);

    RTTransientAlloc a{};
    runtimeAllocTransient(rt, 256, &a);
    EXPECT_NE(a.handle, 0u);
    EXPECT_EQ(a.offset, 0u);
    EXPECT_EQ(a.size, 256u);

    RTTransientAlloc b{};
    runtimeAllocTransient(rt, 256, &b);
    EXPECT_EQ(b.handle, a.handle);  // same transient buffer
    EXPECT_EQ(b.offset, 256u);
    EXPECT_EQ(b.size, 256u);

    // Exceed capacity -> wraps to the beginning
    RTTransientAlloc c{};
    runtimeAllocTransient(rt, 1024, &c);
    EXPECT_EQ(c.offset, 0u);

    runtimeFrameEnd(rt);
    runtimeDestroy(rt);
}

TEST(RTSessionTest, SessionSubmitCountsDraws)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    RTBufferDesc bd{};
    bd.size = 1024;
    bd.usageFlags = 1;
    BufferHandle vbuf = runtimeCreateBuffer(rt, &bd);
    BufferHandle ibuf = runtimeCreateBuffer(rt, &bd);
    ASSERT_NE(vbuf, 0u);
    ASSERT_NE(ibuf, 0u);

    SessionDesc sd = makeSessionDesc(rt, 800, 600);
    SessionHandle s = sessionCreate(&sd);
    ASSERT_NE(s, 0u);

    RTDrawCommand cmds[3]{};
    cmds[0].vertexBufferHandle = vbuf;
    cmds[0].vertexCount = 2;
    cmds[0].topology = RTPrimitiveTopology::LineStrip;
    cmds[0].vertexFormat = static_cast<uint8_t>(RTVertexFormat::P3C3);
    cmds[0].pipelineIndex = runtimeGetDefaultPipeline(rt, DefaultPipeline::ScreenLine);
    cmds[0].space = RenderSpace::Screen;
    cmds[0].sortKey = rtMakeSortKey(0, 0, 0, 0);

    cmds[1].vertexBufferHandle = vbuf;
    cmds[1].indexBufferHandle = ibuf;
    cmds[1].indexCount = 3;
    cmds[1].indexType = 0;
    cmds[1].topology = RTPrimitiveTopology::Triangles;
    cmds[1].vertexFormat = static_cast<uint8_t>(RTVertexFormat::P3C3);
    cmds[1].pipelineIndex = runtimeGetDefaultPipeline(rt, DefaultPipeline::ScreenTri);
    cmds[1].space = RenderSpace::Screen;
    cmds[1].sortKey = rtMakeSortKey(0, 0, 0, 1);

    cmds[2].vertexBufferHandle = vbuf;
    cmds[2].vertexCount = 1;
    cmds[2].topology = RTPrimitiveTopology::Points;
    cmds[2].vertexFormat = static_cast<uint8_t>(RTVertexFormat::P3C3);
    cmds[2].pipelineIndex = runtimeGetDefaultPipeline(rt, DefaultPipeline::ScreenPoint);
    cmds[2].space = RenderSpace::Screen;
    cmds[2].sortKey = rtMakeSortKey(0, 0, 0, 2);

    RTDrawPacket packet{};
    packet.commands = cmds;
    packet.commandCount = 3;
    packet.viewport[2] = 800;
    packet.viewport[3] = 600;

    sessionSubmitDrawCommands(s, &packet);
    sessionPresent(s);

    RTStats stats{};
    sessionGetStats(s, &stats);
    EXPECT_EQ(stats.drawCallCount, 3u);
    EXPECT_EQ(stats.lineCount, 2u);
    EXPECT_EQ(stats.triangleCount, 1u);
    EXPECT_EQ(stats.pointCount, 1u);

    sessionDestroy(s);
    runtimeDestroy(rt);
}

TEST(RTSessionTest, SortKeyBitLayout)
{
    // Layer dominates, then transparency, then depth, then sequence.
    uint64_t a = rtMakeSortKey(0, 0, 0, 0);
    uint64_t b = rtMakeSortKey(0, 0, 0, 1);
    EXPECT_LT(a, b);  // sequence is least significant

    uint64_t c = rtMakeSortKey(0, 0, 10, 0);
    EXPECT_LT(b, c);  // depth dominates sequence

    uint64_t d = rtMakeSortKey(0, 1, 0, 0);
    EXPECT_LT(c, d);  // transparency dominates depth

    uint64_t e = rtMakeSortKey(5, 0, 0, 0);
    EXPECT_LT(d, e);  // layer dominates everything

    // Verify field extraction
    EXPECT_EQ((e >> 56) & 0xFF, 5u);
    EXPECT_EQ((d >> 48) & 0xFF, 1u);
    EXPECT_EQ((c >> 32) & 0xFFFF, 10u);
}

TEST(RTSessionTest, QueryVisibility)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);
    SessionDesc sd = makeSessionDesc(rt, 800, 600);
    SessionHandle s = sessionCreate(&sd);
    ASSERT_NE(s, 0u);

    // [minX, minY, maxX, maxY] per entity
    float aabbs[12] = {
        10.0f, 10.0f, 50.0f, 50.0f,   // inside
        -30.0f, 10.0f, -5.0f, 50.0f,  // fully left of view
        20.0f, 20.0f, 90.0f, 90.0f,   // inside
    };
    float viewBounds[4] = { 0.0f, 0.0f, 100.0f, 100.0f };

    uint32_t outIdx[4] = { 0 };
    RTVisibilityResult res{};
    res.indices = outIdx;
    res.maxCount = 4;
    sessionQueryVisibility(s, aabbs, 3, viewBounds, &res);

    EXPECT_EQ(res.count, 2u);
    if (res.count == 2)
    {
        EXPECT_EQ(res.indices[0], 0u);
        EXPECT_EQ(res.indices[1], 2u);
    }

    sessionDestroy(s);
    runtimeDestroy(rt);
}

TEST(RTSessionTest, MaterialAddUpdate)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    Render::MaterialDesc m{};
    m.color[0] = 1.0f;
    m.lineWidth = 2.0f;
    uint16_t idx = runtimeAddMaterial(rt, &m);
    EXPECT_NE(idx, 0u);  // slot 0 reserved, valid material starts at 1

    m.lineWidth = 3.0f;
    runtimeUpdateMaterial(rt, idx, &m);  // must not crash

    runtimeDestroy(rt);
}

TEST(RTSessionTest, TextureLifecycle)
{
    RuntimeDesc rd = makeNullDesc();
    RuntimeHandle rt = runtimeCreate(&rd);
    ASSERT_NE(rt, 0u);

    RTTextureDesc td{};
    td.width = 64;
    td.height = 64;
    TextureHandle tex = runtimeCreateTexture(rt, &td);
    EXPECT_NE(tex, 0u);

    uint8_t pixels[64 * 64 * 4] = { 0 };
    td.rgba = pixels;
    td.rgbaBytes = sizeof(pixels);
    runtimeUpdateTexture(rt, tex, &td);  // must not crash

    runtimeDestroyTexture(rt, tex);
    runtimeDestroy(rt);
}
