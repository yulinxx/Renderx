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

TEST(RxRuntime, FontLoadReportsUnsupportedInsteadOfSilentSuccess)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    const std::vector<uint8_t> fake(64, 0);
    // 字形图集尚未迁移到新 RHI。静默成功会让调用方以为文本可用，
    // 最终表现为「文字不显示但没有任何错误」。
    EXPECT_EQ(rxFontLoad(runtime, fake.data(), fake.size(), 16.0f),
              RxResult::ErrorUnsupportedBackend);
    EXPECT_EQ(rxFontLoad(runtime, nullptr, 0, 16.0f), RxResult::ErrorInvalidArgument);

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
