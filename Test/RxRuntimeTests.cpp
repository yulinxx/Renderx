/**
 * @file RxRuntimeTests.cpp
 * @brief renderx.h（rx* C API）的契约测试
 *
 * 全部跑在 Null 后端上：Null 后端声明支持所有可选特性，因此上层走的是
 * 真实代码路径而不是降级分支。这里验证的是**契约**——句柄有效性、
 * 帧配对、资源归属、错误码语义，而不是像素结果。
 *
 * 取代已删除的 RuntimeSessionTests.cpp（测的是旧 runtime_session.h API）。
 */

#include <gtest/gtest.h>

#include "render/renderx.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Render::RT;

namespace
{
    /// 收集 DLL 日志，供「失败时必须留下可诊断记录」这类断言使用
    struct LogSink
    {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        static void callback(LogLevel level, const char* message, void* userData)
        {
            auto* sink = static_cast<LogSink*>(userData);
            if (level == LogLevel::Error)
            {
                sink->errors.emplace_back(message ? message : "");
            }
            else if (level == LogLevel::Warn)
            {
                sink->warnings.emplace_back(message ? message : "");
            }
        }
    };

    RuntimeDesc makeRuntimeDesc(LogSink* sink)
    {
        RuntimeDesc desc{};
        desc.abiVersion = RENDERX_ABI_VERSION;
        desc.backend = Backend::Null;
        desc.enableValidation = 1;
        // 1MB：小到能在测试里触发溢出路径，又大到够放常规批次
        desc.transientBufferBytes = 1024 * 1024;
        desc.logCallback = &LogSink::callback;
        desc.logUserData = sink;
        desc.applicationName = "RxRuntimeTests";
        return desc;
    }

    SurfaceDesc makeSurfaceDesc(uint32_t width, uint32_t height, uint8_t depth = 0)
    {
        SurfaceDesc desc{};
        desc.windowKind = NativeWindowKind::None;
        desc.presentMode = PresentMode::Fifo;
        desc.width = width;
        desc.height = height;
        desc.enableDepth = depth;
        return desc;
    }

    /// Runtime + 一个 Surface + 一个 Session 的常规组合
    class RxSessionFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const RuntimeDesc runtimeDesc = makeRuntimeDesc(&sink);
            runtime = rxRuntimeCreate(&runtimeDesc);
            ASSERT_TRUE(rxValid(runtime));

            const SurfaceDesc surfaceDesc = makeSurfaceDesc(800, 600);
            surface = rxSurfaceCreate(runtime, &surfaceDesc);
            ASSERT_TRUE(rxValid(surface));

            SessionDesc sessionDesc{};
            sessionDesc.runtime = runtime;
            sessionDesc.surface = surface;
            sessionDesc.clearColor[3] = 1.0f;
            session = rxSessionCreate(&sessionDesc);
            ASSERT_TRUE(rxValid(session));
        }

        void TearDown() override
        {
            if (rxValid(session))
            {
                rxSessionDestroy(session);
            }
            if (rxValid(surface))
            {
                rxSurfaceDestroy(runtime, surface);
            }
            if (rxValid(runtime))
            {
                rxRuntimeDestroy(runtime);
            }
        }

        LogSink sink;
        RuntimeHandle runtime = RuntimeHandle::Invalid;
        SurfaceHandle surface = SurfaceHandle::Invalid;
        SessionHandle session = SessionHandle::Invalid;
    };
}  // namespace

// ==================== 版本与后端 ====================

TEST(RxStatic, AbiVersionMatchesHeader)
{
    EXPECT_EQ(rxGetAbiVersion(), static_cast<uint32_t>(RENDERX_ABI_VERSION));
}

TEST(RxStatic, NullBackendAlwaysAvailable)
{
    EXPECT_EQ(rxIsBackendAvailable(Backend::Null), 1);
    // Auto 必须永远可用：它的兜底链最终会落到 OpenGL/Null
    EXPECT_EQ(rxIsBackendAvailable(Backend::Auto), 1);
}

TEST(RxStatic, ResultAndBackendNamesAreStable)
{
    EXPECT_STREQ(rxResultName(RxResult::Ok), "Ok");
    EXPECT_STREQ(rxResultName(RxResult::ErrorInvalidHandle), "ErrorInvalidHandle");
    EXPECT_STREQ(rxBackendName(Backend::Null), "Null");
}

TEST(RxRuntime, AbiVersionMismatchIsRejectedAndLogged)
{
    LogSink sink;
    RuntimeDesc desc = makeRuntimeDesc(&sink);
    desc.abiVersion = RENDERX_ABI_VERSION + 1;

    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    EXPECT_FALSE(rxValid(runtime));
    // 静默失败是最糟的形态：调用方必须能从日志里看到「头与 DLL 不一致」
    EXPECT_FALSE(sink.errors.empty());
}

TEST(RxRuntime, NullDescIsRejected)
{
    EXPECT_FALSE(rxValid(rxRuntimeCreate(nullptr)));
}

// ==================== 句柄有效性 ====================

TEST(RxRuntime, DestroyedRuntimeHandleIsRejectedInsteadOfCrashing)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));
    rxRuntimeDestroy(runtime);

    Capabilities caps{};
    // 旧实现在 C API 里裸 reinterpret_cast，这一行会解引用已释放内存
    EXPECT_EQ(rxRuntimeGetCapabilities(runtime, &caps), RxResult::ErrorInvalidHandle);
    EXPECT_FALSE(rxValid(rxBufferCreate(runtime, nullptr)));
}

TEST(RxRuntime, InvalidSessionHandleIsRejected)
{
    FrameStats stats{};
    EXPECT_EQ(rxSessionGetStats(SessionHandle::Invalid, &stats), RxResult::ErrorInvalidHandle);
    EXPECT_EQ(rxSessionBeginFrame(SessionHandle::Invalid), RxResult::ErrorInvalidHandle);
}

TEST(RxRuntime, CapabilitiesReportNullBackend)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    Capabilities caps{};
    ASSERT_EQ(rxRuntimeGetCapabilities(runtime, &caps), RxResult::Ok);
    EXPECT_EQ(caps.backend, Backend::Null);
    EXPECT_GT(caps.maxTextureSize, 0u);
    EXPECT_GE(caps.maxFramesInFlight, 1u);
    EXPECT_STRNE(caps.deviceName, "");

    rxRuntimeDestroy(runtime);
}

// ==================== 内建管线 ====================

TEST(RxRuntime, AllDefaultPipelinesAreAvailable)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    for (uint8_t i = 0; i < static_cast<uint8_t>(DefaultPipeline::Count); ++i)
    {
        const auto kind = static_cast<DefaultPipeline>(i);
        EXPECT_NE(rxPipelineGetDefault(runtime, kind), 0)
            << "内建管线 " << static_cast<int>(i) << " 创建失败";
    }
    // WorldPinned 是本轮新增的第三档渲染空间，单列一条断言以防被顺带删掉
    EXPECT_NE(rxPipelineGetDefault(runtime, DefaultPipeline::WorldPinnedLine), 0);
    EXPECT_NE(rxPipelineGetDefault(runtime, DefaultPipeline::WorldPinnedTri), 0);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, DefaultPipelinesAreDeduplicated)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    // 同一组状态重复请求必须命中缓存返回同一索引。旧实现每帧每笔命令
    // 都往管线表追加一个重复句柄，销毁时重复释放。
    PipelineDesc pipelineDesc{};
    pipelineDesc.topology = PrimitiveTopology::Triangles;
    pipelineDesc.vertexFormat = VertexFormat::P3C4;
    pipelineDesc.blendEnable = 1;
    pipelineDesc.srcBlend = BlendFactor::SrcAlpha;
    pipelineDesc.dstBlend = BlendFactor::OneMinusSrcAlpha;
    pipelineDesc.depthFunc = DepthFunc::LessEqual;

    const uint16_t first = rxPipelineCreate(runtime, &pipelineDesc);
    const uint16_t second = rxPipelineCreate(runtime, &pipelineDesc);
    EXPECT_NE(first, 0);
    EXPECT_EQ(first, second);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, PipelineWithoutBuiltinShaderFailsLoudly)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));
    sink.errors.clear();

    // P3N3 是 3D 网格格式，其 shader 仍用独立 uniform，尚未并入 PushConstants。
    // 必须明确失败并报错，而不是建出一条画不对的管线。
    PipelineDesc pipelineDesc{};
    pipelineDesc.topology = PrimitiveTopology::Triangles;
    pipelineDesc.vertexFormat = VertexFormat::P3N3;
    EXPECT_EQ(rxPipelineCreate(runtime, &pipelineDesc), 0);
    EXPECT_FALSE(sink.errors.empty());

    rxRuntimeDestroy(runtime);
}

// ==================== 缓冲与材质 ====================

TEST(RxRuntime, BufferCreateUploadDestroy)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    BufferDesc bufferDesc{};
    bufferDesc.sizeBytes = 256;
    bufferDesc.cpuWritable = 1;
    const BufferHandle buffer = rxBufferCreate(runtime, &bufferDesc);
    ASSERT_TRUE(rxValid(buffer));

    const std::vector<uint8_t> payload(128, 0x5A);
    EXPECT_EQ(rxBufferUpload(runtime, buffer, 0, payload.size(), payload.data()), RxResult::Ok);
    EXPECT_EQ(rxBufferUpload(runtime, buffer, 0, payload.size(), nullptr),
              RxResult::ErrorInvalidArgument);

    rxBufferDestroy(runtime, buffer);
    sink.warnings.clear();
    // 世代式句柄：销毁后的句柄必须解不出资源，而不是命中被复用的槽位
    EXPECT_EQ(rxBufferUpload(runtime, buffer, 0, payload.size(), payload.data()),
              RxResult::ErrorInvalidArgument);
    rxBufferDestroy(runtime, buffer);
    EXPECT_FALSE(sink.warnings.empty()) << "重复销毁必须留下告警";

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, MaterialIndexZeroIsReserved)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    MaterialDesc material{};
    material.lineWidth = 2.0f;
    material.pointSize = 6.0f;
    material.color[3] = 1.0f;

    const uint16_t index = rxMaterialAdd(runtime, &material);
    EXPECT_NE(index, 0) << "0 号材质保留为「无材质」，不能被分配出去";
    EXPECT_EQ(rxMaterialUpdate(runtime, index, &material), RxResult::Ok);
    EXPECT_EQ(rxMaterialUpdate(runtime, 0, &material), RxResult::ErrorInvalidArgument);
    EXPECT_EQ(rxMaterialUpdate(runtime, 9999, &material), RxResult::ErrorInvalidArgument);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, FontCreateRejectsGarbageData)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    FontHandle font = FontHandle::Invalid;
    const std::vector<uint8_t> fake(64, 0);
    FontDesc fd{};
    fd.data = fake.data();
    fd.dataBytes = fake.size();
    fd.pixelHeight = 16.0f;
    // 64 个零字节不是 TTF。这里必须报错而不是「创建成功但一个字形都出不来」——
    // 后者表现为「文字不显示但没有任何错误」，无从下手。
    EXPECT_EQ(rxFontCreate(runtime, &fd, &font), RxResult::ErrorInvalidArgument);
    EXPECT_FALSE(rxValid(font));

    fd.data = nullptr;
    fd.dataBytes = 0;
    EXPECT_EQ(rxFontCreate(runtime, &fd, &font), RxResult::ErrorInvalidArgument);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, FontGlyphRasterizesAndFillsMetrics)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    std::ifstream file(RENDERX_TEST_FONT_PATH, std::ios::binary);
    ASSERT_TRUE(file.good()) << "缺少测试字体：" << RENDERX_TEST_FONT_PATH;
    const std::vector<uint8_t> ttf((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    ASSERT_FALSE(ttf.empty());

    FontDesc fd{};
    fd.data = ttf.data();
    fd.dataBytes = ttf.size();
    fd.pixelHeight = 16.0f;
    FontHandle font = FontHandle::Invalid;
    ASSERT_EQ(rxFontCreate(runtime, &fd, &font), RxResult::Ok);
    ASSERT_TRUE(rxValid(font));

    FontMetrics metrics{};
    ASSERT_EQ(rxFontMetrics(runtime, font, &metrics), RxResult::Ok);
    EXPECT_GT(metrics.ascent, 0.0f);
    // descent 与 stb_truetype 一致取负值：基线以下的深度
    EXPECT_LT(metrics.descent, 0.0f);
    EXPECT_FLOAT_EQ(metrics.pixelHeight, 16.0f);

    // 图集纹理是普通的公共纹理句柄，可直接填进 DrawCommand::texture
    EXPECT_TRUE(rxValid(rxFontAtlas(runtime, font)));

    GlyphInfo glyph{};
    ASSERT_EQ(rxFontGlyph(runtime, font, U'0', &glyph), RxResult::Ok);
    EXPECT_GT(glyph.advance, 0.0f);
    EXPECT_GT(glyph.width, 0.0f);
    EXPECT_GT(glyph.height, 0.0f);
    EXPECT_LT(glyph.u0, glyph.u1);
    EXPECT_LT(glyph.v0, glyph.v1);
    // bearingY 以基线为原点、y 向下为正，字形主体在基线之上，故为负
    EXPECT_LT(glyph.bearingY, 0.0f);

    // 空格有步进但没有像素：不该产出四边形
    GlyphInfo space{};
    ASSERT_EQ(rxFontGlyph(runtime, font, U' ', &space), RxResult::Ok);
    EXPECT_GT(space.advance, 0.0f);
    EXPECT_FLOAT_EQ(space.width, 0.0f);
    EXPECT_FLOAT_EQ(space.height, 0.0f);

    // 同一码点第二次查询走缓存，结果必须逐字段一致
    GlyphInfo again{};
    ASSERT_EQ(rxFontGlyph(runtime, font, U'0', &again), RxResult::Ok);
    EXPECT_FLOAT_EQ(again.u0, glyph.u0);
    EXPECT_FLOAT_EQ(again.advance, glyph.advance);

    // 上传是幂等的：脏区清空后再 flush 是空操作
    EXPECT_EQ(rxFontFlushAtlas(runtime, font), RxResult::Ok);
    EXPECT_EQ(rxFontFlushAtlas(runtime, font), RxResult::Ok);

    rxFontDestroy(runtime, font);
    // 销毁后句柄立即失效（世代式句柄）
    EXPECT_EQ(rxFontMetrics(runtime, font, &metrics), RxResult::ErrorInvalidHandle);
    EXPECT_FALSE(rxValid(rxFontAtlas(runtime, font)));

    rxRuntimeDestroy(runtime);
}

// ==================== 表面与会话归属 ====================

TEST(RxRuntime, ZeroSizedSurfaceIsRejected)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    const SurfaceDesc surfaceDesc = makeSurfaceDesc(0, 0);
    EXPECT_FALSE(rxValid(rxSurfaceCreate(runtime, &surfaceDesc)));

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, MultipleSurfacesShareOneRuntime)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    // 多窗口的正确形态：一个 Runtime（一个设备 + 共享资源）+ N 个 Surface。
    // 旧实现设备与窗口一对一绑死，第二个窗口只能再建一个 Runtime。
    const SurfaceDesc a = makeSurfaceDesc(640, 480);
    const SurfaceDesc b = makeSurfaceDesc(1280, 720);
    const SurfaceHandle surfaceA = rxSurfaceCreate(runtime, &a);
    const SurfaceHandle surfaceB = rxSurfaceCreate(runtime, &b);
    ASSERT_TRUE(rxValid(surfaceA));
    ASSERT_TRUE(rxValid(surfaceB));
    EXPECT_NE(surfaceA, surfaceB);

    SessionDesc sessionDesc{};
    sessionDesc.runtime = runtime;
    sessionDesc.surface = surfaceA;
    const SessionHandle sessionA = rxSessionCreate(&sessionDesc);
    sessionDesc.surface = surfaceB;
    const SessionHandle sessionB = rxSessionCreate(&sessionDesc);
    ASSERT_TRUE(rxValid(sessionA));
    ASSERT_TRUE(rxValid(sessionB));

    // 两个窗口在同一 wall-clock 帧内各自完整走一遍
    EXPECT_EQ(rxSessionBeginFrame(sessionA), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(sessionA), RxResult::Ok);
    EXPECT_EQ(rxSessionBeginFrame(sessionB), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(sessionB), RxResult::Ok);

    rxSessionDestroy(sessionA);
    rxSessionDestroy(sessionB);
    rxSurfaceDestroy(runtime, surfaceA);
    rxSurfaceDestroy(runtime, surfaceB);
    rxRuntimeDestroy(runtime);
}

TEST_F(RxSessionFixture, SecondSessionOnSameSurfaceIsRejected)
{
    sink.errors.clear();
    SessionDesc sessionDesc{};
    sessionDesc.runtime = runtime;
    sessionDesc.surface = surface;
    // 两个 Session 画同一个表面会互相覆盖，属于调用方错误
    EXPECT_FALSE(rxValid(rxSessionCreate(&sessionDesc)));
    EXPECT_FALSE(sink.errors.empty());
}

TEST_F(RxSessionFixture, SurfaceWithBoundSessionCannotBeDestroyed)
{
    sink.errors.clear();
    rxSurfaceDestroy(runtime, surface);
    EXPECT_FALSE(sink.errors.empty());
    // 表面仍然可用：销毁被拒绝，而不是留下一个半死的对象
    EXPECT_EQ(rxSurfaceResize(runtime, surface, 1024, 768), RxResult::Ok);
}

TEST_F(RxSessionFixture, SurfaceHandleFromAnotherRuntimeIsRejected)
{
    LogSink otherSink;
    const RuntimeDesc otherDesc = makeRuntimeDesc(&otherSink);
    const RuntimeHandle other = rxRuntimeCreate(&otherDesc);
    ASSERT_TRUE(rxValid(other));

    sink.errors.clear();
    // 句柄本质是指针，来自其他 Runtime 的表面必须被拦下而不是直接解引用
    EXPECT_EQ(rxSurfaceResize(other, surface, 100, 100), RxResult::ErrorInvalidArgument);

    rxRuntimeDestroy(other);
}

// ==================== 帧流程 ====================

TEST_F(RxSessionFixture, FrameMustBePaired)
{
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::ErrorUnknown) << "未 BeginFrame 就 EndFrame";
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    sink.errors.clear();
    EXPECT_NE(rxSessionBeginFrame(session), RxResult::Ok) << "同一帧内重复 BeginFrame";
    EXPECT_FALSE(sink.errors.empty());
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

TEST_F(RxSessionFixture, TransientAllocOnlyValidInsideFrame)
{
    TransientAlloc alloc{};
    EXPECT_EQ(rxSessionAllocTransient(session, 64, &alloc), RxResult::ErrorUnknown);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionAllocTransient(session, 64, &alloc), RxResult::Ok);
    EXPECT_TRUE(rxValid(alloc.buffer));
    EXPECT_NE(alloc.cpuPtr, nullptr);
    EXPECT_EQ(alloc.sizeBytes, 64u);

    // 连续两次分配不得重叠
    TransientAlloc second{};
    ASSERT_EQ(rxSessionAllocTransient(session, 64, &second), RxResult::Ok);
    EXPECT_NE(second.offset, alloc.offset);
    EXPECT_GE(second.offset, alloc.offset + alloc.sizeBytes);

    EXPECT_EQ(rxSessionAllocTransient(session, 0, &alloc), RxResult::ErrorInvalidArgument);
    EXPECT_EQ(rxSessionAllocTransient(session, 64, nullptr), RxResult::ErrorInvalidArgument);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

TEST_F(RxSessionFixture, SubmitOutsideFrameIsRejected)
{
    DrawPacket packet{};
    DrawCommand command{};
    command.vertexCount = 3;
    packet.commands = &command;
    packet.commandCount = 1;
    EXPECT_EQ(rxSessionSubmit(session, &packet), RxResult::ErrorUnknown);
    EXPECT_EQ(rxSessionSubmit(session, nullptr), RxResult::ErrorInvalidArgument);
}

TEST_F(RxSessionFixture, SubmitCountsDrawCallsAndPrimitives)
{
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3C4) * 6, &alloc),
              RxResult::Ok);

    DrawCommand commands[2]{};
    commands[0].vertexBuffer = alloc.buffer;
    commands[0].vertexOffset = alloc.offset;
    commands[0].vertexCount = 3;
    commands[0].topology = PrimitiveTopology::Triangles;
    commands[0].space = RenderSpace::World;
    commands[0].vertexFormat = VertexFormat::P3C4;
    commands[0].indexType = IndexType::None;
    commands[0].sortKey = rxMakeSortKey(10, 0, 0, 0);

    commands[1] = commands[0];
    commands[1].topology = PrimitiveTopology::Lines;
    commands[1].vertexCount = 4;
    commands[1].sortKey = rxMakeSortKey(200, 1, 0, 1);

    DrawPacket packet{};
    packet.commands = commands;
    packet.commandCount = 2;
    packet.viewMatrix[0] = 1.0f;
    packet.viewMatrix[5] = 1.0f;
    packet.viewMatrix[10] = 1.0f;
    packet.viewMatrix[15] = 1.0f;
    packet.viewport[2] = 800.0f;
    packet.viewport[3] = 600.0f;

    ASSERT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);

    FrameStats stats{};
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    EXPECT_EQ(stats.drawCallCount, 2u);
    EXPECT_EQ(stats.triangleCount, 1u);
    EXPECT_EQ(stats.lineCount, 2u);
    EXPECT_GT(stats.pipelineSwitches, 0u);

    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 统计每帧重置，不能累加
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    EXPECT_EQ(stats.drawCallCount, 0u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

TEST_F(RxSessionFixture, InvalidVertexBufferIsSkippedNotFatal)
{
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    sink.warnings.clear();

    DrawCommand command{};
    command.vertexBuffer = BufferHandle::Invalid;
    command.vertexCount = 3;
    command.topology = PrimitiveTopology::Triangles;
    command.vertexFormat = VertexFormat::P3C4;
    command.indexType = IndexType::None;

    DrawPacket packet{};
    packet.commands = &command;
    packet.commandCount = 1;

    // 一条坏命令不应该让整帧失败，但必须留下告警
    EXPECT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
    FrameStats stats{};
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    EXPECT_EQ(stats.drawCallCount, 0u);
    EXPECT_FALSE(sink.warnings.empty());

    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

TEST_F(RxSessionFixture, WorldPinnedCommandUsesPinnedVertexFormat)
{
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3O2C4) * 3, &alloc),
              RxResult::Ok);

    DrawCommand command{};
    command.vertexBuffer = alloc.buffer;
    command.vertexOffset = alloc.offset;
    command.vertexCount = 3;
    command.topology = PrimitiveTopology::Triangles;
    command.space = RenderSpace::WorldPinned;
    command.vertexFormat = VertexFormat::P3O2C4;
    command.indexType = IndexType::None;

    DrawPacket packet{};
    packet.commands = &command;
    packet.commandCount = 1;

    ASSERT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
    FrameStats stats{};
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    EXPECT_EQ(stats.drawCallCount, 1u);

    // WorldPinned 只有 P3O2C4 一种顶点格式：用 P3C4 提交必须被跳过，
    // 因为顶点里没有像素偏移字段，做不出定尺寸效果。
    sink.warnings.clear();
    command.vertexFormat = VertexFormat::P3C4;
    ASSERT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    EXPECT_EQ(stats.drawCallCount, 1u) << "P3C4 + WorldPinned 不应产生绘制";
    EXPECT_FALSE(sink.warnings.empty());

    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

TEST_F(RxSessionFixture, ResizeIsReflectedInNextFrame)
{
    ASSERT_EQ(rxSurfaceResize(runtime, surface, 1024, 768), RxResult::Ok);
    EXPECT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

// ==================== 可见性查询 ====================

TEST_F(RxSessionFixture, QueryVisibilityFiltersAndTruncates)
{
    // (minX, minY, maxX, maxY) 紧凑排列
    const float aabbs[] = {
        0.0f,   0.0f,   10.0f,  10.0f,   // 命中
        100.0f, 100.0f, 110.0f, 110.0f,  // 不命中
        -5.0f,  -5.0f,  1.0f,   1.0f,    // 命中（部分相交）
        50.0f,  0.0f,   60.0f,  5.0f,    // 不命中
    };
    const float viewBounds[4] = { -10.0f, -10.0f, 20.0f, 20.0f };

    uint32_t indices[4] = {};
    VisibilityResult result{};
    result.indices = indices;
    result.capacity = 4;

    ASSERT_EQ(rxSessionQueryVisibility(session, aabbs, 4, viewBounds, &result), RxResult::Ok);
    ASSERT_EQ(result.count, 2u);
    EXPECT_EQ(indices[0], 0u);
    EXPECT_EQ(indices[1], 2u);

    // 容量不足不是错误：调用方按 count == capacity 判断是否需要扩容重试
    result.capacity = 1;
    result.count = 0;
    sink.warnings.clear();
    ASSERT_EQ(rxSessionQueryVisibility(session, aabbs, 4, viewBounds, &result), RxResult::Ok);
    EXPECT_EQ(result.count, 1u);
    EXPECT_FALSE(sink.warnings.empty());

    result.indices = nullptr;
    EXPECT_EQ(rxSessionQueryVisibility(session, aabbs, 4, viewBounds, &result),
              RxResult::ErrorInvalidArgument);
}

// ==================== 排序键 ====================

TEST(RxSortKey, LayerDominatesThenTransparencyThenDepthThenSeq)
{
    EXPECT_LT(rxMakeSortKey(10, 0, 0, 0), rxMakeSortKey(11, 0, 0, 0));
    EXPECT_LT(rxMakeSortKey(10, 0, 0xFFFF, 0xFFFF), rxMakeSortKey(10, 1, 0, 0));
    EXPECT_LT(rxMakeSortKey(10, 1, 5, 0xFFFF), rxMakeSortKey(10, 1, 6, 0));
    EXPECT_LT(rxMakeSortKey(10, 1, 5, 7), rxMakeSortKey(10, 1, 5, 8));
    EXPECT_EQ(rxMakeSortKey(0, 0, 0, 0), 0u);
    // 覆盖层约定 layer=200 / transparent=1，必须排在常规图元之后
    EXPECT_LT(rxMakeSortKey(100, 0, 0xFFFF, 0xFFFF), rxMakeSortKey(200, 1, 0, 0));
}

// ==================== 持久几何仓 ====================

namespace
{
    GeometryStoreDesc makeStoreDesc(uint64_t initialBytes, uint64_t maxBytes, uint32_t granularity)
    {
        GeometryStoreDesc desc{};
        desc.initialBytes = initialBytes;
        desc.maxBytes = maxBytes;
        desc.granularity = granularity;
        desc.forIndices = 0;
        return desc;
    }

    /// 只需要 Runtime 的用例（几何仓与绘制列表的所有权在 Runtime 上）
    class RxIncrementalFixture : public RxSessionFixture
    {
    };
}  // namespace

TEST_F(RxIncrementalFixture, GeometryStoreAllocatesAlignedBlocks)
{
    const GeometryStoreDesc desc = makeStoreDesc(4096, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock first{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 100, &first), RxResult::Ok);
    EXPECT_EQ(first.offset, 0u);
    EXPECT_EQ(first.sizeBytes, 100u) << "sizeBytes 应是请求值，不是对齐后的值";
    EXPECT_TRUE(rxValid(first.buffer));

    // 第二块必须落在按粒度对齐的位置：顶点属性最坏对齐是 16 字节，
    // 起始偏移不对齐会在部分驱动上静默画错。
    GeometryBlock second{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 100, &second), RxResult::Ok);
    EXPECT_EQ(second.offset, 256u);
    EXPECT_EQ(second.buffer, first.buffer);

    GeometryStoreStats stats{};
    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    EXPECT_EQ(stats.capacityBytes, 4096u);
    EXPECT_EQ(stats.usedBytes, 512u);
    EXPECT_EQ(stats.blockCount, 2u);

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryFreeCoalescesAdjacentHoles)
{
    const GeometryStoreDesc desc = makeStoreDesc(4096, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock blocks[3]{};
    for (GeometryBlock& block : blocks)
    {
        ASSERT_EQ(rxGeometryAlloc(runtime, store, 256, &block), RxResult::Ok);
    }

    GeometryStoreStats stats{};
    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    // 三块紧邻分配之后只剩尾部一个空洞
    ASSERT_EQ(stats.freeRangeCount, 1u);

    // 释放中间那块，再释放第一块：两个空洞相邻，必须合并成一个。
    // 不合并的话反复 alloc/free 会把空闲表打成碎屑，first-fit 退化。
    ASSERT_EQ(rxGeometryFree(runtime, store, blocks[1].id), RxResult::Ok);
    ASSERT_EQ(rxGeometryFree(runtime, store, blocks[0].id), RxResult::Ok);

    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    // 头部合并成 [0,512)，尾部仍是 [768,4096)：中间的 blocks[2] 还占着，
    // 因此是 2 个区间而不是 1 个。
    EXPECT_EQ(stats.freeRangeCount, 2u);
    EXPECT_EQ(stats.usedBytes, 256u);

    // 真正验证合并：请求 512 字节应当落回偏移 0。
    // 若两个空洞没合并，first-fit 只能找到 256 的碎片，只好去尾部。
    GeometryBlock reused{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 512, &reused), RxResult::Ok);
    EXPECT_EQ(reused.offset, 0u);

    // 已释放的块不能再写：句柄是世代式的，重复释放/写入必须报错而不是越界
    EXPECT_EQ(rxGeometryFree(runtime, store, blocks[0].id), RxResult::ErrorInvalidHandle);
    const uint8_t byte = 0;
    EXPECT_EQ(rxGeometryWrite(runtime, store, blocks[0].id, 0, 1, &byte),
              RxResult::ErrorInvalidHandle);

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryAllocReportsGrowthAndKeepsBufferHandleStable)
{
    // 初始只有 1024 字节：第二次分配必然触发扩容
    const GeometryStoreDesc desc = makeStoreDesc(1024, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock first{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 1024, &first), RxResult::Ok);

    GeometryBlock second{};
    // 正数结果不是失败：分配成功，但底层缓冲已被替换
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 256, &second), RxResult::ErrorGeometryStoreGrown);

    GeometryStoreStats stats{};
    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    EXPECT_EQ(stats.growCount, 1u);
    EXPECT_EQ(stats.capacityBytes, 2048u) << "应翻倍增长，线性增长会退化成 O(n²) 次搬迁";

    // 句柄数值保持稳定：槽位被原地改写，因此调用方已发出的所有
    // GeometryBlock::buffer 仍然指向新缓冲，不必逐块刷新。
    EXPECT_EQ(rxGeometryStoreGetBuffer(runtime, store), first.buffer);
    EXPECT_EQ(second.buffer, first.buffer);

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryStoreRefusesToExceedMaxBytes)
{
    const GeometryStoreDesc desc = makeStoreDesc(256, 512, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock block{};
    ASSERT_NE(rxGeometryAlloc(runtime, store, 256, &block), RxResult::ErrorOutOfMemory);
    ASSERT_NE(rxGeometryAlloc(runtime, store, 256, &block), RxResult::ErrorOutOfMemory);

    sink.errors.clear();
    EXPECT_EQ(rxGeometryAlloc(runtime, store, 256, &block), RxResult::ErrorOutOfMemory);
    EXPECT_FALSE(sink.errors.empty()) << "达到上限必须留下可诊断记录";

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryWriteRejectsOutOfBlockRange)
{
    const GeometryStoreDesc desc = makeStoreDesc(4096, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock block{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 64, &block), RxResult::Ok);

    // 越界判定用的是**对齐后的块大小**（256），而不是请求的 64：
    // 对齐产生的尾部同样属于该块，写进去不会踩到别人。
    std::vector<uint8_t> payload(256, 0xAB);
    EXPECT_EQ(rxGeometryWrite(runtime, store, block.id, 0, 256, payload.data()), RxResult::Ok);
    sink.errors.clear();
    EXPECT_EQ(rxGeometryWrite(runtime, store, block.id, 1, 256, payload.data()),
              RxResult::ErrorInvalidArgument);
    EXPECT_FALSE(sink.errors.empty());

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryDirtyRangesMergeAcrossSmallGaps)
{
    const GeometryStoreDesc desc = makeStoreDesc(1u << 16, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock block{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 16384, &block), RxResult::Ok);

    const uint8_t payload[16] = {};
    // 两处小改动，间隙 1984 字节（小于 4KB 合并阈值）
    ASSERT_EQ(rxGeometryWrite(runtime, store, block.id, 0, 16, payload), RxResult::Ok);
    ASSERT_EQ(rxGeometryWrite(runtime, store, block.id, 2000, 16, payload), RxResult::Ok);

    GeometryStoreStats stats{};
    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    EXPECT_EQ(stats.dirtyBytesThisFrame, 32u) << "flush 前脏区仍是两段各 16 字节";

    ASSERT_EQ(rxGeometryFlush(runtime, store), RxResult::Ok);
    ASSERT_EQ(rxGeometryStoreGetStats(runtime, store, &stats), RxResult::Ok);
    EXPECT_EQ(stats.dirtyBytesThisFrame, 0u);

    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, GeometryUploadBytesReflectDirtyMerging)
{
    const GeometryStoreDesc desc = makeStoreDesc(1u << 16, 1u << 20, 256);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &desc);
    ASSERT_TRUE(rxValid(store));

    GeometryBlock block{};
    ASSERT_EQ(rxGeometryAlloc(runtime, store, 16384, &block), RxResult::Ok);
    // 建仓时的首次写入先刷掉，免得混进本帧统计
    const uint8_t payload[16] = {};
    ASSERT_EQ(rxGeometryWrite(runtime, store, block.id, 0, 16, payload), RxResult::Ok);
    ASSERT_EQ(rxGeometryFlush(runtime, store), RxResult::Ok);

    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxGeometryWrite(runtime, store, block.id, 0, 16, payload), RxResult::Ok);
    ASSERT_EQ(rxGeometryWrite(runtime, store, block.id, 2000, 16, payload), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);

    FrameStats stats{};
    ASSERT_EQ(rxSessionGetStats(session, &stats), RxResult::Ok);
    // 合并后一次传 2016 字节，而不是两次共 32 字节。
    // 刻意的过度传输：多传 2KB 远比多一次 writeBuffer（含驱动同步）便宜。
    EXPECT_EQ(stats.geometryUploadBytes, 2016u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxGeometryStoreDestroy(runtime, store);
}

// ==================== 保留式绘制列表 ====================

namespace
{
    /// 一条最简可绘制命令：P3C3 三角形列表，顶点来自给定缓冲
    DrawCommand makeListCommand(BufferHandle buffer, uint32_t vertexOffset, uint32_t vertexCount,
                                uint64_t sortKey, PrimitiveTopology topology)
    {
        DrawCommand command{};
        command.vertexBuffer = buffer;
        command.sortKey = sortKey;
        command.vertexOffset = vertexOffset;
        command.vertexCount = vertexCount;
        command.topology = topology;
        command.space = RenderSpace::World;
        command.vertexFormat = VertexFormat::P3C3;
        command.indexType = IndexType::None;
        return command;
    }

    /// 建一个够大的顶点缓冲，供绘制列表用例引用
    BufferHandle makeVertexBuffer(RuntimeHandle runtime, uint64_t bytes)
    {
        BufferDesc desc{};
        desc.sizeBytes = bytes;
        desc.cpuWritable = 1;
        return rxBufferCreate(runtime, &desc);
    }
}  // namespace

TEST_F(RxIncrementalFixture, DrawListTracksEntryCountAcrossUpsertRemoveClear)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 8;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const DrawCommand command = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 5, &command, nullptr), RxResult::Ok);
    // 同一槽位重复 upsert 是更新，不是新增
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 5, &command, nullptr), RxResult::Ok);

    DrawListStats stats{};
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.entryCount, 2u);

    ASSERT_EQ(rxDrawListRemove(runtime, list, 5), RxResult::Ok);
    // 移除不存在的槽位是调用方错误
    EXPECT_EQ(rxDrawListRemove(runtime, list, 5), RxResult::ErrorInvalidArgument);
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.entryCount, 1u);

    ASSERT_EQ(rxDrawListClear(runtime, list), RxResult::Ok);
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.entryCount, 0u);

    // 槽号必须紧凑分配：直接拿实体 64 位 ID 当槽号会撑爆稠密数组
    sink.errors.clear();
    EXPECT_EQ(rxDrawListUpsert(runtime, list, 1u << 25, &command, nullptr),
              RxResult::ErrorInvalidArgument);
    EXPECT_FALSE(sink.errors.empty());

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListCullsByAabbAndCountsIt)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableCulling = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const DrawCommand inside = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const DrawCommand outside = makeListCommand(buffer, 512, 3, 2, PrimitiveTopology::Triangles);
    const float insideBox[4] = { 0.0f, 0.0f, 10.0f, 10.0f };
    const float outsideBox[4] = { 1000.0f, 1000.0f, 1010.0f, 1010.0f };
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &inside, insideBox), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &outside, outsideBox), RxResult::Ok);
    // 无 AABB 的条目永不被剔除（覆盖层通常如此）
    const DrawCommand overlay = makeListCommand(buffer, 1024, 3, 3, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 2, &overlay, nullptr), RxResult::Ok);

    const float viewBounds[4] = { -50.0f, -50.0f, 50.0f, 50.0f };
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, viewBounds), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 1u);
    EXPECT_EQ(frame.drawCallCount, 2u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 传 nullptr 关闭剔除：三条全画
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 0u);
    EXPECT_EQ(frame.drawCallCount, 3u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListSortsOnlyWhenOrderCanChange)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    DrawCommand command = makeListCommand(buffer, 0, 3, 10, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    DrawListStats stats{};
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    const uint32_t sortsAfterFirstFrame = stats.sortCount;
    EXPECT_EQ(sortsAfterFirstFrame, 1u);

    // 第二帧什么都没改：不应重排。「每帧不重排」正是保留式列表
    // 相对 DrawPacket 的收益所在，退化成每帧排序不会报错，只是变慢。
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.sortCount, sortsAfterFirstFrame);

    // 只改顶点范围（sortKey 不变）同样不触发重排
    command.vertexCount = 6;
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, nullptr), RxResult::Ok);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.sortCount, sortsAfterFirstFrame);

    // 改 sortKey 才重排
    command.sortKey = 20;
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, nullptr), RxResult::Ok);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.sortCount, sortsAfterFirstFrame + 1);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListMergesContiguousListTopologies)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableMerging = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    // P3C3 步长 24：3 个顶点 = 72 字节，因此第二条从 72 开始才算连续
    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
    ASSERT_EQ(stride, 24u);
    const DrawCommand a = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const DrawCommand b = makeListCommand(buffer, 3 * stride, 3, 2, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &a, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &b, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.mergedDrawCount, 1u);
    EXPECT_EQ(frame.drawCallCount, 1u);
    EXPECT_EQ(frame.triangleCount, 2u) << "合并后仍应画满两个三角形";
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListNeverMergesStripOrLoopTopologies)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableMerging = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
    // 顶点区间连续、状态完全相同 —— 唯一的区别是拓扑是 LineStrip。
    // 合并会把两条独立折线连起来，多画一段；这种错误在密集图形里
    // 几乎看不出来，因此必须在这里锁住。
    const DrawCommand a = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::LineStrip);
    const DrawCommand b = makeListCommand(buffer, 3 * stride, 3, 2, PrimitiveTopology::LineStrip);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &a, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &b, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.mergedDrawCount, 0u);
    EXPECT_EQ(frame.drawCallCount, 2u);
    // 两条各 3 顶点的折线各 2 段，合并会变成 5 段
    EXPECT_EQ(frame.lineCount, 4u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListDoesNotMergeAcrossVertexGaps)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableMerging = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    // 第二条从 512 开始，与第一条之间有空隙：合并会把空隙里的字节
    // 当成顶点画出来。
    const DrawCommand a = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const DrawCommand b = makeListCommand(buffer, 512, 3, 2, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &a, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &b, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.mergedDrawCount, 0u);
    EXPECT_EQ(frame.drawCallCount, 2u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListRejectsForeignAndDestroyedHandles)
{
    DrawListDesc listDesc{};
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    rxDrawListDestroy(runtime, list);
    // 世代式句柄：销毁后同一数值立即失效，不会误命中新对象
    EXPECT_EQ(rxDrawListClear(runtime, list), RxResult::ErrorInvalidHandle);
    EXPECT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::ErrorInvalidHandle);

    GeometryStoreStats storeStats{};
    EXPECT_EQ(rxGeometryStoreGetStats(runtime, GeometryStoreHandle::Invalid, &storeStats),
              RxResult::ErrorInvalidHandle);
}

// ==================== 像素读回 ====================

TEST_F(RxIncrementalFixture, ReadPixelsRequiresOpenFrameAndSufficientCapacity)
{
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);

    // 帧外读回：EndFrame 之后后备缓冲已交给呈现，内容不再保证有效。
    // 明确报错比「尽力读一次」好——读到上一帧或空白画面更难排查。
    sink.errors.clear();
    EXPECT_EQ(rxSessionReadPixels(session, 0, 0, 4, 4, pixels.data(), pixels.size()),
              RxResult::ErrorUnknown);
    EXPECT_FALSE(sink.errors.empty());

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    sink.errors.clear();
    EXPECT_EQ(rxSessionReadPixels(session, 0, 0, 4, 4, pixels.data(), 16),
              RxResult::ErrorInvalidArgument);
    EXPECT_FALSE(sink.errors.empty()) << "缓冲过小必须说明需要多少字节";

    EXPECT_EQ(rxSessionReadPixels(session, 0, 0, 4, 4, nullptr, pixels.size()),
              RxResult::ErrorInvalidArgument);

    // 读回后必须能继续正常收尾：readPixels 内部拆了 RenderPass 又重开，
    // 漏掉重开会让 EndFrame 的 endRenderPass 变成未配对调用。
    EXPECT_EQ(rxSessionReadPixels(session, 0, 0, 4, 4, pixels.data(), pixels.size()), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
}

