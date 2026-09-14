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
#include "rt/rxIncremental.h"
#include "shader/shaderLibrary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
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
    // 世界空间贴图：位图图元的唯一通道
    EXPECT_NE(rxPipelineGetDefault(runtime, DefaultPipeline::WorldTextured), 0);
    EXPECT_NE(rxPipelineGetDefault(runtime, DefaultPipeline::WorldGlyphSdf), 0);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, WorldTexturedDiffersFromScreenTextured)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    // 两者片元相同但顶点着色器不同（一个乘 uView、一个把位置当像素坐标），
    // 必须是两条独立管线。若哪天顶点格式判据写错而落到同一条，贴图会画在
    // 屏幕坐标上、不随视图变换——是静默画错而非报错，所以在此设一道断言。
    EXPECT_NE(rxPipelineGetDefault(runtime, DefaultPipeline::WorldTextured),
              rxPipelineGetDefault(runtime, DefaultPipeline::ScreenTextured));

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, WorldGlyphSdfDiffersFromWorldTextured)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    const uint16_t sdf = rxPipelineGetDefault(runtime, DefaultPipeline::WorldGlyphSdf);

    // 管线索引非 0 同时也证明了 world_glyph_sdf_p3t2c4.frag 真的编译通过——
    // GLSL 的语法/语义错误只在建管线时才暴露，编译期查不出来。
    EXPECT_NE(sdf, 0);

    // 两者 (格式, 空间, 拓扑) 完全相同，只有片元不同：一个把 R8 当距离场，
    // 一个当 RGBA 采样。若落到同一条管线，文字会变成纯红色块——静默画错，
    // 所以在此设断言。与 ScreenGlyph / ScreenTextured 同理。
    EXPECT_NE(sdf, rxPipelineGetDefault(runtime, DefaultPipeline::WorldTextured));
    EXPECT_NE(sdf, rxPipelineGetDefault(runtime, DefaultPipeline::ScreenGlyph));

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, TexturedFormatsRejectMismatchedSpace)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    // rxPipelineCreate 没有 space 字段，一律按 World 建（见 Runtime::createPipeline）。
    // P2T2C4 的顶点着色器只有屏幕空间实现，所以这条组合必须失败返回 0，
    // 而不是静默建出一条把世界坐标当像素坐标用的管线。
    // 屏幕空间贴图请走 rxPipelineGetDefault(ScreenTextured/ScreenGlyph)。
    PipelineDesc screenFmt{};
    screenFmt.topology = PrimitiveTopology::Triangles;
    screenFmt.vertexFormat = VertexFormat::P2T2C4;
    EXPECT_EQ(rxPipelineCreate(runtime, &screenFmt), 0);

    // P3T2C4 是世界空间格式，同一条路径应当成功
    PipelineDesc worldFmt{};
    worldFmt.topology = PrimitiveTopology::Triangles;
    worldFmt.vertexFormat = VertexFormat::P3T2C4;
    EXPECT_NE(rxPipelineCreate(runtime, &worldFmt), 0);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, P3T2C4StrideMatchesLayout)
{
    // 顶点结构在宿主侧手写（BitmapQuadBuilder 的 BVertex），
    // stride 对不上会让整批顶点错位，这条锁住 ABI。
    EXPECT_EQ(rxVertexStride(VertexFormat::P3T2C4), 36u);
}

TEST(RxRuntime, Lighting3DDescMatchesStd140Layout)
{
    // sizeof 相等不代表布局相等：字段顺序调换后总大小可能不变，
    // 而 shader 仍按旧偏移取值——表现为「高光颜色被当成光照方向」这类
    // 无从下手的画面错乱。这里逐字段锁住 rx_lighting_3d.glsl 注释里的偏移表。
    EXPECT_EQ(sizeof(Lighting3DDesc), 160u);
    EXPECT_EQ(sizeof(DirectionalLight3D), 32u) << "std140 结构体尺寸向上取整到 16 的倍数";

    EXPECT_EQ(offsetof(Lighting3DDesc, ambientColor), 0u);
    EXPECT_EQ(offsetof(Lighting3DDesc, ambientEnabled), 12u);
    EXPECT_EQ(offsetof(Lighting3DDesc, ambientIntensity), 16u);
    EXPECT_EQ(offsetof(Lighting3DDesc, doubleSided), 20u);
    EXPECT_EQ(offsetof(Lighting3DDesc, specularEnabled), 24u);
    EXPECT_EQ(offsetof(Lighting3DDesc, specularIntensity), 28u);
    // 三个方向光必须落在 16 字节边界上，否则 std140 会插入隐式填充
    EXPECT_EQ(offsetof(Lighting3DDesc, key), 32u);
    EXPECT_EQ(offsetof(Lighting3DDesc, fill), 64u);
    EXPECT_EQ(offsetof(Lighting3DDesc, rim), 96u);
    EXPECT_EQ(offsetof(Lighting3DDesc, viewPos), 128u);
    EXPECT_EQ(offsetof(Lighting3DDesc, minBrightness), 140u);
    EXPECT_EQ(offsetof(Lighting3DDesc, exposure), 144u);

    EXPECT_EQ(offsetof(DirectionalLight3D, direction), 0u);
    EXPECT_EQ(offsetof(DirectionalLight3D, enabled), 12u);
    EXPECT_EQ(offsetof(DirectionalLight3D, color), 16u);
    EXPECT_EQ(offsetof(DirectionalLight3D, intensity), 28u);
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

TEST(RxRuntime, Mesh3DPipelineResolvesToRealShaders)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));
    sink.errors.clear();

    // P3N3 是 3D 网格格式。5.0 起它有真实的内建 shader（mesh_3d_p3n3.*），
    // 因此必须能建出管线——此前这里返回 0 是「3D 未收口」的占位行为。
    PipelineDesc pipelineDesc{};
    pipelineDesc.topology = PrimitiveTopology::Triangles;
    pipelineDesc.vertexFormat = VertexFormat::P3N3;
    EXPECT_NE(rxPipelineCreate(runtime, &pipelineDesc), 0);
    EXPECT_TRUE(sink.errors.empty());

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, Default3DPipelinesAreDistinct)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    const uint16_t mesh = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    const uint16_t wire = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3DWire);
    const uint16_t highlight = rxPipelineGetDefault(runtime, DefaultPipeline::Highlight3D);
    const uint16_t gizmo = rxPipelineGetDefault(runtime, DefaultPipeline::Gizmo3D);
    EXPECT_NE(mesh, 0);
    EXPECT_NE(wire, 0);
    EXPECT_NE(highlight, 0);
    EXPECT_NE(gizmo, 0);

    // 线框只有 fillMode 与实心不同。fillMode 必须进管线缓存键，
    // 否则两者命中同一条管线——先建的赢，另一条静默画错。
    EXPECT_NE(mesh, wire) << "fillMode 未进管线键：线框与实心塌缩成同一条管线";

    // 高亮走 P3C4 + 世界三角形，与 2D 覆盖层的 WorldTri4 同格式同空间同拓扑，
    // 只有深度状态与填充模式不同，因此必须是独立的一条。
    EXPECT_NE(highlight, rxPipelineGetDefault(runtime, DefaultPipeline::WorldTri4))
        << "深度状态/填充模式未进管线键：3D 高亮与 2D 覆盖三角形塌缩成同一条管线";

    // 手柄与高亮同格式同空间同拓扑，只有 fillMode 与深度偏移不同；
    // 手柄与 WorldTri4 则只差深度状态与偏移。两条都必须独立。
    EXPECT_NE(gizmo, highlight) << "3D 手柄与高亮塌缩成同一条管线";
    EXPECT_NE(gizmo, rxPipelineGetDefault(runtime, DefaultPipeline::WorldTri4))
        << "深度偏移未进管线键：3D 手柄与 2D 覆盖三角形塌缩成同一条管线";

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, DepthBiasEntersPipelineKey)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    // 深度偏移是管线固定状态（Vulkan 的 VkPipelineRasterizationStateCreateInfo、
    // Metal 的 setDepthBias:），运行时改不了，因此必须进缓存键。
    PipelineDesc plain{};
    plain.topology = PrimitiveTopology::Triangles;
    plain.vertexFormat = VertexFormat::P3C4;
    plain.depthTest = 1;

    PipelineDesc biased = plain;
    biased.depthBiasConstant = 1.0f;
    biased.depthBiasSlope = 1.0f;

    const uint16_t plainIndex = rxPipelineCreate(runtime, &plain);
    const uint16_t biasedIndex = rxPipelineCreate(runtime, &biased);
    EXPECT_NE(plainIndex, 0);
    EXPECT_NE(biasedIndex, 0);
    EXPECT_NE(plainIndex, biasedIndex) << "depthBias 未进管线键：有偏移与无偏移塌缩成一条";
    // 同一组状态重复请求仍要命中缓存
    EXPECT_EQ(rxPipelineCreate(runtime, &biased), biasedIndex);

    rxRuntimeDestroy(runtime);
}

TEST(RxRuntime, WireframePipelineDedupesByFillMode)
{
    LogSink sink;
    const RuntimeDesc desc = makeRuntimeDesc(&sink);
    const RuntimeHandle runtime = rxRuntimeCreate(&desc);
    ASSERT_TRUE(rxValid(runtime));

    PipelineDesc solid{};
    solid.topology = PrimitiveTopology::Triangles;
    solid.vertexFormat = VertexFormat::P3N3;
    solid.fillMode = FillMode::Solid;

    PipelineDesc wire = solid;
    wire.fillMode = FillMode::Wireframe;

    const uint16_t solidIndex = rxPipelineCreate(runtime, &solid);
    const uint16_t wireIndex = rxPipelineCreate(runtime, &wire);
    EXPECT_NE(solidIndex, 0);
    EXPECT_NE(wireIndex, 0);
    EXPECT_NE(solidIndex, wireIndex);
    // 同一组状态重复请求仍要命中缓存
    EXPECT_EQ(rxPipelineCreate(runtime, &wire), wireIndex);

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

TEST(RxRuntime, FontSdfModeOutsetsGlyphByPadding)
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

    constexpr uint32_t kPadding = 8;

    // 同一字体、同一字号、同一码点，只改 sdfPadding
    FontDesc coverageDesc{};
    coverageDesc.data = ttf.data();
    coverageDesc.dataBytes = ttf.size();
    coverageDesc.pixelHeight = 32.0f;
    FontHandle coverageFont = FontHandle::Invalid;
    ASSERT_EQ(rxFontCreate(runtime, &coverageDesc, &coverageFont), RxResult::Ok);

    FontDesc sdfDesc = coverageDesc;
    sdfDesc.sdfPadding = kPadding;
    FontHandle sdfFont = FontHandle::Invalid;
    ASSERT_EQ(rxFontCreate(runtime, &sdfDesc, &sdfFont), RxResult::Ok);

    GlyphInfo coverage{};
    GlyphInfo sdf{};
    ASSERT_EQ(rxFontGlyph(runtime, coverageFont, U'0', &coverage), RxResult::Ok);
    ASSERT_EQ(rxFontGlyph(runtime, sdfFont, U'0', &sdf), RxResult::Ok);
    ASSERT_GT(coverage.width, 0.0f);
    ASSERT_GT(sdf.width, 0.0f);

    // 距离场向四周各外扩 padding 像素：四边形因此比墨迹大一圈。
    // 这一圈不能省——轮廓外侧的距离值正是抗锯齿过渡所需的数据，
    // 裁掉就退化成硬边（旧实现的症状之一）。
    EXPECT_FLOAT_EQ(sdf.width, coverage.width + 2.0f * kPadding);
    EXPECT_FLOAT_EQ(sdf.height, coverage.height + 2.0f * kPadding);
    // bearing 同步左上外移，否则字会整体偏移 padding 个像素
    EXPECT_FLOAT_EQ(sdf.bearingX, coverage.bearingX - static_cast<float>(kPadding));
    EXPECT_FLOAT_EQ(sdf.bearingY, coverage.bearingY - static_cast<float>(kPadding));
    // 步进只取决于字体的水平度量，与图集内容无关，必须一致：
    // 若这里也被 padding 撑大，字距会随 padding 变化。
    EXPECT_FLOAT_EQ(sdf.advance, coverage.advance);

    rxFontDestroy(runtime, sdfFont);
    rxFontDestroy(runtime, coverageFont);
    rxRuntimeDestroy(runtime);
}


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

    /// 轴对齐立方体视锥：六个平面都朝盒内，判据可手算，适合做剔除用例
    RxFrustum makeBoxFrustum(float minValue, float maxValue)
    {
        const float planes[6][4] = {
            { 1.0f, 0.0f, 0.0f, -minValue },   // x >= minValue
            { -1.0f, 0.0f, 0.0f, maxValue },   // x <= maxValue
            { 0.0f, 1.0f, 0.0f, -minValue },   // y >= minValue
            { 0.0f, -1.0f, 0.0f, maxValue },   // y <= maxValue
            { 0.0f, 0.0f, 1.0f, -minValue },   // z >= minValue
            { 0.0f, 0.0f, -1.0f, maxValue },   // z <= maxValue
        };
        RxFrustum frustum{};
        for (int i = 0; i < 6; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                frustum.planes[i][j] = planes[i][j];
            }
        }
        return frustum;
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

    // 槽号必须紧凑分配：直接拿图元 64 位 ID 当槽号会撑爆稠密数组
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

TEST_F(RxIncrementalFixture, DrawListCullsByFrustum3D)
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
    const DrawCommand straddling = makeListCommand(buffer, 1024, 3, 3, PrimitiveTopology::Triangles);
    const RxAabb3 insideBox{ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    const RxAabb3 outsideBox{ 1000.0f, 1000.0f, 1000.0f, 1001.0f, 1001.0f, 1001.0f };
    // 跨越 x = 10 这条平面：判据是「盒子与视锥有交集」，它必须留下
    const RxAabb3 straddlingBox{ 5.0f, 0.0f, 0.0f, 200.0f, 1.0f, 1.0f };
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 0, &inside, &insideBox), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 1, &outside, &outsideBox), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 2, &straddling, &straddlingBox), RxResult::Ok);
    // 无包围盒的条目永不被剔除
    const DrawCommand overlay = makeListCommand(buffer, 2048, 3, 4, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 3, &overlay, nullptr), RxResult::Ok);

    const RxFrustum frustum = makeBoxFrustum(-10.0f, 10.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList3D(session, list, &frustum), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 1u);
    EXPECT_EQ(frame.drawCallCount, 3u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 传 nullptr 关闭剔除：四条全画
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList3D(session, list, nullptr), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 0u);
    EXPECT_EQ(frame.drawCallCount, 4u);
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

TEST_F(RxIncrementalFixture, DrawListBatchesTightBlocksFromRealAllocator)
{
    // 回归：合批要求顶点区间字节连续，而几何仓的块大小是按粒度向上取整的，
    // 因此粒度必须能整除顶点步长（P3C3 步长 24，粒度取 8），块才会紧排。
    //
    // 这个用例刻意走**真实分配器**，而不是手工构造 offset —— 已有的那批合批
    // 用例全是手工 offset，正是它们掩盖了「分配器与合批前置条件互相矛盾」：
    // 上线实测 70 万图元 0 次合批、每帧 70 万次 draw call（每次约 0.4us，
    // 光提交就 0.33 秒，装不进一帧）。
    const GeometryStoreDesc storeDesc = makeStoreDesc(1u << 16, 1u << 20, 8);
    const GeometryStoreHandle store = rxGeometryStoreCreate(runtime, &storeDesc);
    ASSERT_TRUE(rxValid(store));

    DrawListDesc listDesc{};
    listDesc.initialCapacity = 8;
    listDesc.enableMerging = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
    ASSERT_EQ(stride, 24u);
    // 6 个顶点 = 144 字节：既是粒度 8 的倍数（块紧排），也是 3 的倍数
    // （TriangleList 的图元边界，否则会被拼接守卫拦下）
    constexpr uint32_t kVertexCount = 6;
    const uint32_t bytes = kVertexCount * stride;

    const BufferHandle buffer = rxGeometryStoreGetBuffer(runtime, store);
    ASSERT_TRUE(rxValid(buffer));

    constexpr uint32_t kPieces = 3;
    uint32_t expectedOffset = 0;
    for (uint32_t i = 0; i < kPieces; ++i)
    {
        GeometryBlock block{};
        ASSERT_EQ(rxGeometryAlloc(runtime, store, bytes, &block), RxResult::Ok);
        // 紧排：第 i 块必须紧接前一块，中间不能有粒度补白
        EXPECT_EQ(block.offset, expectedOffset)
            << "粒度不能整除步长时块之间会出现空隙，合批必然一次都不发生";
        expectedOffset += bytes;

        const DrawCommand command =
            makeListCommand(buffer, block.offset, kVertexCount, 1, PrimitiveTopology::Triangles);
        ASSERT_EQ(rxDrawListUpsert(runtime, list, i, &command, nullptr), RxResult::Ok);
    }

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.mergedDrawCount, kPieces - 1) << "紧密相邻的三段应合成一次 draw";
    EXPECT_EQ(frame.drawCallCount, 1u);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxGeometryStoreDestroy(runtime, store);
}

TEST_F(RxIncrementalFixture, DrawListRefusesToFuseAcrossPrimitiveBoundary)
{
    // 并段会把两段顶点首尾拼成**一个区间**，而列表型拓扑是按顺序成组消费顶点的：
    // 前一段的顶点数不是完整图元数时，它的「多余顶点」会和后一段的首顶点配成
    // 一个本不存在的图元。这类错误只表现为多画，在密集图形里几乎看不出来。
    //
    // 注意区分两件事：
    //   - 成批（一次提交多段）：跨段边界天然安全，图元数不变；
    //   - 并段（合成一个区间）：只有边界落在完整图元上才安全。
    // 因此这里用**图元数**而不是 drawCallCount 来判定——两种做法都只出 1 次 draw。
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableMerging = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);

    // 5 个顶点的 LineList：末尾那个顶点本来会被 GL 忽略，一旦与下一段并成一个
    // 区间，它就会和下一段的首顶点连成一条多余的线（3 条 → 4 条）。
    const DrawCommand oddLines = makeListCommand(buffer, 0, 5, 1, PrimitiveTopology::Lines);
    const DrawCommand nextLines = makeListCommand(buffer, 5 * stride, 3, 1, PrimitiveTopology::Lines);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &oddLines, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &nextLines, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    // 5/2 + 3/2 = 2 + 1 = 3 条线。若被并成一个 8 顶点的区间就是 4 条。
    EXPECT_EQ(frame.lineCount, 3u) << "奇数顶点的 LineList 不能与后续段并成一个区间";
    EXPECT_EQ(frame.drawCallCount, 1u) << "两段仍应成批为一次提交";
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 对照：前一段 6 个顶点落在 3 的倍数上，边界合法，允许并成一个区间。
    // 分开画是 2 + 1 = 3 个三角形，并成 9 顶点也是 3 个——因此这里用 drawCallCount
    // 无法区分，改用一个「并段才会多画」的构造：4 + 5 顶点。
    ASSERT_EQ(rxDrawListClear(runtime, list), RxResult::Ok);
    const DrawCommand trisAligned = makeListCommand(buffer, 0, 6, 1, PrimitiveTopology::Triangles);
    const DrawCommand trisNext = makeListCommand(buffer, 6 * stride, 3, 1, PrimitiveTopology::Triangles);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &trisAligned, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &trisNext, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.triangleCount, 3u) << "6 与 3 都落在 3 的倍数上，并段前后都是 3 个三角形";
    EXPECT_EQ(frame.drawCallCount, 1u);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

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
    // 折线**可以成批**（一次提交多段，每段各自成折线，边界天然安全），
    // 但**不能并段**：并成一段会把两条独立折线连起来、多画一段。
    // 这种错误在密集图形里几乎看不出来，因此必须在这里锁住。
    const DrawCommand a = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::LineStrip);
    const DrawCommand b = makeListCommand(buffer, 3 * stride, 3, 2, PrimitiveTopology::LineStrip);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &a, nullptr), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &b, nullptr), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, nullptr), RxResult::Ok);
    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    // 两条各 3 顶点的折线各 2 段。并成一段会变成 5 段 —— 这条断言是真正的护栏。
    EXPECT_EQ(frame.lineCount, 4u);
    // 成批后只出一次 draw
    EXPECT_EQ(frame.drawCallCount, 1u);
    EXPECT_EQ(frame.mergedDrawCount, 1u);
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

TEST_F(RxIncrementalFixture, DrawListDoesNotMergeAcrossCulledEntries)
{
    // 回归：合批要求「状态相同 + 顶点区间连续」，而部分可见会把这个连续性打断。
    // 三条线段顶点区间首尾相接（0 / 3 / 6 个顶点），中间那条被剔除时，
    // 若把首尾两条合并成一个 draw，GPU 会把中间那条的顶点也一起画出来。
    // 这类「静默多画」在密集图形里几乎看不出来，必须用测试锁住。
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableMerging = 1;
    listDesc.enableCulling = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
    const DrawCommand first = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const DrawCommand middle = makeListCommand(buffer, 3 * stride, 3, 2, PrimitiveTopology::Triangles);
    const DrawCommand last = makeListCommand(buffer, 6 * stride, 3, 3, PrimitiveTopology::Triangles);

    const RxAabb3 inside{ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    const RxAabb3 outside{ 1000.0f, 1000.0f, 1000.0f, 1001.0f, 1001.0f, 1001.0f };
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 0, &first, &inside), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 1, &middle, &outside), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 2, &last, &inside), RxResult::Ok);

    // 3D 视锥走线性路径（2D 走空间索引），这里正是新加的合批预计算逻辑
    const RxFrustum frustum = makeBoxFrustum(-10.0f, 10.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList3D(session, list, &frustum), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 1u);
    // 首尾两段的顶点区间被剔除的那条隔开，因此只能各成一段。
    // 若并成一个 9 顶点的区间，GPU 会把中间那条的顶点也画出来（3 个三角形）。
    EXPECT_EQ(frame.triangleCount, 2u) << "中间条目被剔除，首尾不得并成一个区间";
    EXPECT_EQ(frame.mergedDrawCount, 1u) << "两段仍应成批为一次提交";
    EXPECT_EQ(frame.drawCallCount, 1u);
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

// ======================================================================
// 2D 空间索引选层护栏
//
// selectGridLevel 的口径是「返回层边长 >= maxExtent 的最细层」，这是保证
// 单个图元至多覆盖 2x2 格（插入/摘除代价可控）的前提。此前它被误读为有
// off-by-one，这里用边界断言把行为锁死，让「有没有 bug」机器可判。
// ======================================================================

TEST(RxDrawListGridTest, SelectGridLevelPicksFinestFittingLayer)
{
    // 恰好等于层边长 → 该层（相等即装得下）；略超 → 下一层
    EXPECT_EQ(detail::selectGridLevel(1.0f), 0);
    EXPECT_EQ(detail::selectGridLevel(256.0f), 0);
    EXPECT_EQ(detail::selectGridLevel(256.5f), 1);
    EXPECT_EQ(detail::selectGridLevel(512.0f), 1);
    EXPECT_EQ(detail::selectGridLevel(512.5f), 2);
    EXPECT_EQ(detail::selectGridLevel(1024.0f), 2);
    EXPECT_EQ(detail::selectGridLevel(1024.5f), 3);
    EXPECT_EQ(detail::selectGridLevel(2048.0f), 3);
}

TEST(RxDrawListGridTest, SelectGridLevelClampsToLastLayer)
{
    // 最大层边长 = kGridBaseCellSize << (kGridLevelCount-1)，超过它只能落在最后一层
    const float maxCellSize =
        detail::kGridBaseCellSize * static_cast<float>(1 << (detail::kGridLevelCount - 1));
    EXPECT_EQ(detail::selectGridLevel(maxCellSize), detail::kGridLevelCount - 1);
    EXPECT_EQ(detail::selectGridLevel(maxCellSize * 4.0f), detail::kGridLevelCount - 1);
    EXPECT_EQ(detail::selectGridLevel(1e9f), detail::kGridLevelCount - 1);
}

TEST(RxDrawListGridTest, SelectGridLevelFitsAndIsFinest)
{
    // 不变式：所选层 cellSize >= maxExtent（未被 clamp 时），且再细一层就装不下
    // —— 这正是「最细的能容纳它的层」。
    const float maxCellSize =
        detail::kGridBaseCellSize * static_cast<float>(1 << (detail::kGridLevelCount - 1));
    for (float extent : { 0.5f, 1.0f, 100.0f, 256.0f, 256.1f, 400.0f, 512.0f, 513.0f,
                          1000.0f, 1024.0f, 2000.0f, 4096.0f, 100000.0f })
    {
        const uint16_t level = detail::selectGridLevel(extent);
        const float cellSize = detail::kGridBaseCellSize * static_cast<float>(1 << level);
        if (extent <= maxCellSize)
        {
            EXPECT_LE(extent, cellSize) << "extent=" << extent << " level=" << level;
        }
        if (level > 0)
        {
            const float finerCellSize =
                detail::kGridBaseCellSize * static_cast<float>(1 << (level - 1));
            EXPECT_GT(extent, finerCellSize) << "extent=" << extent << " level=" << level;
        }
    }
}

TEST(RxDrawListGridTest, SelectGridLevelMonotonicInExtent)
{
    // 单调性：extent 增大时层号不减（选层不回退，索引分层稳定）
    uint16_t prev = 0;
    for (float extent : { 1.0f, 100.0f, 256.0f, 300.0f, 512.0f, 600.0f, 1024.0f,
                          1500.0f, 2048.0f, 5000.0f, 100000.0f })
    {
        const uint16_t level = detail::selectGridLevel(extent);
        EXPECT_GE(level, prev) << "extent=" << extent;
        prev = level;
    }
}

TEST_F(RxIncrementalFixture, DrawListSpatialIndexMatchesBruteForceCulling)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 512;
    listDesc.enableCulling = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096 * 64);
    ASSERT_TRUE(rxValid(buffer));

    // 固定种子保证可复现；坐标与尺寸覆盖负值、多尺度（触发不同层与跨格）。
    constexpr uint32_t kCount = 256;
    float boxes[kCount][4]{};
    std::mt19937 rng(20260813u);
    std::uniform_real_distribution<float> coord(-10000.0f, 10000.0f);
    std::uniform_real_distribution<float> extent(0.5f, 4000.0f);

    for (uint32_t i = 0; i < kCount; ++i)
    {
        const float x = coord(rng);
        const float y = coord(rng);
        boxes[i][0] = x;
        boxes[i][1] = y;
        boxes[i][2] = x + extent(rng);
        boxes[i][3] = y + extent(rng);
    }

    // 顶点偏移不连续，避免合批干扰 drawCallCount 与 visibleCount 的对应。
    for (uint32_t i = 0; i < kCount; ++i)
    {
        const DrawCommand command =
            makeListCommand(buffer, i * 1024, 3, 100 + i, PrimitiveTopology::Triangles);
        ASSERT_EQ(rxDrawListUpsert(runtime, list, i, &command, boxes[i]), RxResult::Ok);
    }

    const float viewBounds[4] = { -500.0f, -500.0f, 500.0f, 500.0f };
    // 参考实现：与 DLL 内相同的矩形判交，独立算一遍期望值。
    uint32_t expectedVisible = 0;
    for (uint32_t i = 0; i < kCount; ++i)
    {
        const bool disjoint = boxes[i][2] < viewBounds[0] || boxes[i][0] > viewBounds[2] ||
                              boxes[i][3] < viewBounds[1] || boxes[i][1] > viewBounds[3];
        if (!disjoint)
        {
            expectedVisible += 1;
        }
    }

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, viewBounds), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, kCount - expectedVisible);
    EXPECT_EQ(frame.drawCallCount, expectedVisible);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    DrawListStats stats{};
    ASSERT_EQ(rxDrawListGetStats(runtime, list, &stats), RxResult::Ok);
    EXPECT_EQ(stats.visibleCount, expectedVisible);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListMovesEntryAcrossCellsOnUpsert)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 4;
    listDesc.enableCulling = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const DrawCommand command = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const float viewBounds[4] = { -50.0f, -50.0f, 50.0f, 50.0f };
    const float insideBox[4] = { 0.0f, 0.0f, 10.0f, 10.0f };
    const float outsideBox[4] = { 5000.0f, 5000.0f, 5010.0f, 5010.0f };

    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, insideBox), RxResult::Ok);

    // 移动到视口外：同一槽位改包围盒，索引必须从旧格摘除并插到新格
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, outsideBox), RxResult::Ok);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, viewBounds), RxResult::Ok);
    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 1u);
    EXPECT_EQ(frame.drawCallCount, 0u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 再移回来：必须能重新命中
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &command, insideBox), RxResult::Ok);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, viewBounds), RxResult::Ok);
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    EXPECT_EQ(frame.culledCommandCount, 0u);
    EXPECT_EQ(frame.drawCallCount, 1u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

TEST_F(RxIncrementalFixture, DrawListKeepsNon2DEntriesVisibleIn2DView)
{
    DrawListDesc listDesc{};
    listDesc.initialCapacity = 8;
    listDesc.enableCulling = 1;
    const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
    ASSERT_TRUE(rxValid(list));

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    const DrawCommand inside = makeListCommand(buffer, 0, 3, 1, PrimitiveTopology::Triangles);
    const DrawCommand outside = makeListCommand(buffer, 512, 3, 2, PrimitiveTopology::Triangles);
    const DrawCommand mesh3D = makeListCommand(buffer, 1024, 3, 3, PrimitiveTopology::Triangles);
    const DrawCommand overlay = makeListCommand(buffer, 2048, 3, 4, PrimitiveTopology::Triangles);

    const float insideBox[4] = { 0.0f, 0.0f, 10.0f, 10.0f };
    const float outsideBox[4] = { 1000.0f, 1000.0f, 1010.0f, 1010.0f };
    const RxAabb3 meshBox{ 0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 10.0f };
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 0, &inside, insideBox), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 1, &outside, outsideBox), RxResult::Ok);
    // 3D 条目与无包围盒条目：2D 视口下类型不匹配，一律不裁、照常画。
    ASSERT_EQ(rxDrawListUpsert3D(runtime, list, 2, &mesh3D, &meshBox), RxResult::Ok);
    ASSERT_EQ(rxDrawListUpsert(runtime, list, 3, &overlay, nullptr), RxResult::Ok);

    const float viewBounds[4] = { -50.0f, -50.0f, 50.0f, 50.0f };
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionSubmitDrawList(session, list, viewBounds), RxResult::Ok);

    FrameStats frame{};
    ASSERT_EQ(rxSessionGetStats(session, &frame), RxResult::Ok);
    // 只有 outside（2D 且在视口外）被裁；3D 与无包围盒条目照常画
    EXPECT_EQ(frame.culledCommandCount, 1u);
    EXPECT_EQ(frame.drawCallCount, 3u);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    rxDrawListDestroy(runtime, list);
    rxBufferDestroy(runtime, buffer);
}

// ==================== 内置 shader 的 uniform 块 ====================

namespace
{
    /// 取出 shader 里所有「无实例名」uniform 块的成员名
    ///
    /// 只支持本仓 shader 实际使用的写法：`uniform Name { ... };`，块内是一层
    /// 平铺的 `类型 名字;` 声明（结构体在块外声明）。故意不做通用 GLSL 解析。
    std::vector<std::string> collectAnonymousBlockMembers(const std::string& src)
    {
        std::vector<std::string> members;
        size_t pos = 0;
        while ((pos = src.find("uniform", pos)) != std::string::npos)
        {
            const size_t open = src.find('{', pos);
            const size_t close = src.find('}', open == std::string::npos ? pos : open);
            if (open == std::string::npos || close == std::string::npos)
            {
                break;
            }

            // 块后紧跟 ';' 才是无实例名的块；有实例名时成员不进全局命名空间
            size_t after = close + 1;
            while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])))
            {
                ++after;
            }
            const bool anonymous = after < src.size() && src[after] == ';';

            if (anonymous)
            {
                const std::string body = src.substr(open + 1, close - open - 1);
                size_t stmtBegin = 0;
                while (stmtBegin < body.size())
                {
                    const size_t semi = body.find(';', stmtBegin);
                    if (semi == std::string::npos)
                    {
                        break;
                    }
                    // 取 ';' 之前的最后一个标识符，即成员名
                    size_t end = semi;
                    while (end > stmtBegin && !std::isalnum(static_cast<unsigned char>(body[end - 1])) &&
                           body[end - 1] != '_')
                    {
                        --end;
                    }
                    size_t begin = end;
                    while (begin > stmtBegin && (std::isalnum(static_cast<unsigned char>(body[begin - 1])) ||
                                                 body[begin - 1] == '_'))
                    {
                        --begin;
                    }
                    if (end > begin)
                    {
                        members.push_back(body.substr(begin, end - begin));
                    }
                    stmtBegin = semi + 1;
                }
            }
            pos = close + 1;
        }
        return members;
    }
}  // namespace

TEST(RxShaderLibrary, AnonymousUniformBlockMembersDoNotCollide)
{
    // 回归：PushConstants 与 FrameUniforms 都曾有一个叫 uPad0 的占位成员。
    // 两者都是「无实例名」的 uniform 块，成员名进的是全局命名空间，因此同时
    // 包含二者的 mesh_3d_p3n3.frag 编译失败：
    //   "Field name 'uPad0' of interface block without instance name
    //    'FrameUniforms' would shadow a previous declaration"
    // Mesh3D / Mesh3DWire 管线于是建不出来，所有 P3N3 命令被跳过 ——
    // 表面现象是「导入 3D 模型完全不显示，选中只剩高亮线框」（线框走 P3C4，
    // 不含 FrameUniforms，照常编译）。
    //
    // 这条只能在 GL 上暴露：Null 后端不编译 GLSL，Mesh3D 的其它用例照样绿。
    // 因此这里直接检查嵌进二进制的展开后源码，不依赖任何 GL 上下文。
    const uint32_t total = Render::shader::count();
    ASSERT_GT(total, 0u);

    uint32_t checked = 0;
    for (uint32_t i = 0; i < total; ++i)
    {
        if (Render::shader::languageAt(i) != Render::shader::Language::Glsl)
        {
            continue;
        }
        const char* name = Render::shader::nameAt(i);
        ASSERT_NE(name, nullptr);
        const char* source = Render::shader::glslSource(name);
        ASSERT_NE(source, nullptr) << name;

        const std::vector<std::string> members = collectAnonymousBlockMembers(source);
        for (size_t a = 0; a < members.size(); ++a)
        {
            for (size_t b = a + 1; b < members.size(); ++b)
            {
                EXPECT_NE(members[a], members[b])
                    << name << " 的无实例名 uniform 块里有重名成员 \"" << members[a]
                    << "\"，GLSL 会报 would shadow a previous declaration，整条管线建不出来";
            }
        }
        ++checked;
    }
    EXPECT_GT(checked, 0u);
}

// ==================== 3D 光照上传时机 ====================

TEST_F(RxIncrementalFixture, BeginFrameUploadsLighting3DBeforeAnyMeshDraw)
{
    // 回归：光照 UBO 的上传曾被误放在 rxSessionReadPixels 的「重开 RenderPass」
    // 分支里，正常渲染路径一次都不会走到。后果是绑定组从未创建，
    // recordCommands 跳过绑定，mesh_3d_p3n3.frag 读到全零 UBO
    // （无光 + 曝光 0）后把网格画成纯黑 —— 深色背景上就是「模型看不见」，
    // 而选中高亮走不吃光照的 Highlight3D，反而正常显示。
    Lighting3DDesc lighting{};
    lighting.ambientColor[0] = 1.0f;
    lighting.ambientColor[1] = 1.0f;
    lighting.ambientColor[2] = 1.0f;
    lighting.ambientEnabled = 1;
    lighting.ambientIntensity = 0.3f;
    lighting.key.enabled = 1;
    lighting.key.direction[1] = 1.0f;
    lighting.key.color[0] = 1.0f;
    lighting.key.color[1] = 1.0f;
    lighting.key.color[2] = 1.0f;
    lighting.key.intensity = 1.0f;
    lighting.exposure = 1.0f;
    rxSessionSetLighting3D(session, &lighting);

    const BufferHandle buffer = makeVertexBuffer(runtime, 4096);
    ASSERT_TRUE(rxValid(buffer));

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    sink.warnings.clear();
    sink.errors.clear();

    DrawCommand command{};
    command.vertexBuffer = buffer;
    command.vertexCount = 3;
    command.topology = PrimitiveTopology::Triangles;
    command.space = RenderSpace::World;
    command.vertexFormat = VertexFormat::P3N3;
    command.indexType = IndexType::None;
    command.pipelineIndex = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(command.pipelineIndex, 0);

    DrawPacket packet{};
    packet.commands = &command;
    packet.commandCount = 1;
    ASSERT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);

    // 断言的是「绑定组已就绪」，而不是像素值：Null 后端不出图，
    // 但缺绑定组这条路径会留下唯一一条可辨识的告警。
    for (const std::string& warning : sink.warnings)
    {
        EXPECT_EQ(warning.find("lighting uniforms are unavailable"), std::string::npos)
            << "光照 UBO 必须在 BeginFrame 里就绑好：" << warning;
    }
    EXPECT_TRUE(sink.errors.empty());

    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);
    rxBufferDestroy(runtime, buffer);
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

// ==================== 离屏渲染 ====================

TEST_F(RxIncrementalFixture, OffscreenRenderTargetCreateAndRead)
{
    // 创建渲染目标纹理
    Render::RT::RenderTargetDesc rtDesc{};
    rtDesc.width = 256;
    rtDesc.height = 256;
    rtDesc.usage = Render::RT::TextureUsageFlag::ColorAttachment | Render::RT::TextureUsageFlag::TransferSrc;
    TextureHandle rtTexture = rxTextureCreateRenderTarget(runtime, &rtDesc);
    ASSERT_TRUE(rxValid(rtTexture));

    // 设置 Session 使用离屏渲染目标
    EXPECT_EQ(rxSessionSetRenderTarget(session, rtTexture, TextureHandle::Invalid, 256, 256), RxResult::Ok);

    // 开始帧（离屏模式不需要 acquireNextImage）
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    // 提交一个简单的三角形
    DrawCommand cmd{};
    cmd.vertexBuffer = BufferHandle::Invalid;
    cmd.indexBuffer = BufferHandle::Invalid;
    cmd.sortKey = rxMakeSortKey(0, 0, 0, 0);
    cmd.vertexCount = 3;
    cmd.topology = PrimitiveTopology::Triangles;
    cmd.space = RenderSpace::Screen;
    cmd.vertexFormat = VertexFormat::P3C4;
    cmd.pipelineIndex = rxPipelineGetDefault(runtime, DefaultPipeline::ScreenTri);

    // 分配瞬态内存
    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, 3 * rxVertexStride(VertexFormat::P3C4), &alloc), RxResult::Ok);
    cmd.vertexBuffer = alloc.buffer;
    cmd.vertexOffset = alloc.offset;

    // 写入三角形顶点（屏幕空间，像素坐标）
    struct Vertex { float x, y, z, r, g, b, a; };
    Vertex* verts = static_cast<Vertex*>(alloc.cpuPtr);
    verts[0] = { 100.0f, 100.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };  // 红
    verts[1] = { 200.0f, 100.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f };  // 绿
    verts[2] = { 150.0f, 200.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };  // 蓝

    DrawPacket packet{};
    packet.commands = &cmd;
    packet.commandCount = 1;
    packet.enableCulling = 0;
    packet.frameId = 1;
    std::memset(packet.viewMatrix, 0, sizeof(packet.viewMatrix));
    packet.viewport[0] = 0; packet.viewport[1] = 0; packet.viewport[2] = 256; packet.viewport[3] = 256;

    EXPECT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 从离屏纹理读回像素（不要求在帧内）
    std::vector<uint8_t> pixels(256 * 256 * 4);
    EXPECT_EQ(rxSessionReadPixelsFromTexture(session, rtTexture, 0, 0, 256, 256, pixels.data(), pixels.size()),
              RxResult::Ok);

    // Null 后端不实际渲染，只验证 API 调用成功
    // 真实 GPU 后端测试由 RenderxGLTests（GL 后端）覆盖

    // 恢复到交换链渲染
    EXPECT_EQ(rxSessionSetRenderTarget(session, TextureHandle::Invalid, TextureHandle::Invalid, 0, 0), RxResult::Ok);

    // 清理
    rxTextureDestroy(runtime, rtTexture);
}

TEST_F(RxIncrementalFixture, OffscreenWithDepthAttachment)
{
    // 创建带深度附件的渲染目标
    Render::RT::RenderTargetDesc colorDesc{};
    colorDesc.width = 128;
    colorDesc.height = 128;
    colorDesc.usage = Render::RT::TextureUsageFlag::ColorAttachment | Render::RT::TextureUsageFlag::TransferSrc;
    TextureHandle colorTex = rxTextureCreateRenderTarget(runtime, &colorDesc);
    ASSERT_TRUE(rxValid(colorTex));

    Render::RT::RenderTargetDesc depthDesc{};
    depthDesc.width = 128;
    depthDesc.height = 128;
    depthDesc.usage = Render::RT::TextureUsageFlag::DepthStencilAttachment;
    TextureHandle depthTex = rxTextureCreateRenderTarget(runtime, &depthDesc);
    ASSERT_TRUE(rxValid(depthTex));

    // 设置 Session 使用离屏渲染目标（带深度）
    EXPECT_EQ(rxSessionSetRenderTarget(session, colorTex, depthTex, 128, 128), RxResult::Ok);

    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    // 提交 3D 网格（需要深度测试）
    DrawCommand cmd{};
    cmd.sortKey = rxMakeSortKey(0, 0, 0, 0);
    cmd.topology = PrimitiveTopology::Triangles;
    cmd.space = RenderSpace::World;
    cmd.vertexFormat = VertexFormat::P3N3;
    cmd.pipelineIndex = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, 3 * rxVertexStride(VertexFormat::P3N3), &alloc), RxResult::Ok);
    cmd.vertexBuffer = alloc.buffer;
    cmd.vertexOffset = alloc.offset;

    struct Vertex3D { float x, y, z, nx, ny, nz; };
    Vertex3D* verts = static_cast<Vertex3D*>(alloc.cpuPtr);
    verts[0] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    verts[1] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    verts[2] = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };

    DrawPacket packet{};
    packet.commands = &cmd;
    packet.commandCount = 1;
    packet.enableCulling = 0;
    packet.frameId = 1;
    float viewMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    std::memcpy(packet.viewMatrix, viewMatrix, sizeof(viewMatrix));
    packet.viewport[0] = 0; packet.viewport[1] = 0; packet.viewport[2] = 128; packet.viewport[3] = 128;

    EXPECT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
    EXPECT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    // 读回验证
    std::vector<uint8_t> pixels(128 * 128 * 4);
    EXPECT_EQ(rxSessionReadPixelsFromTexture(session, colorTex, 0, 0, 128, 128, pixels.data(), pixels.size()),
              RxResult::Ok);

    // 恢复
    EXPECT_EQ(rxSessionSetRenderTarget(session, TextureHandle::Invalid, TextureHandle::Invalid, 0, 0), RxResult::Ok);

    // 清理
    rxTextureDestroy(runtime, colorTex);
    rxTextureDestroy(runtime, depthTex);
}

// ==================== 百万级性能基准 ====================

namespace
{
    /// 毫秒计时（steady_clock，不受系统时钟调整影响）
    double elapsedMs(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    }
}  // namespace

TEST_F(RxIncrementalFixture, DrawListMillionPrimitiveCullingBaseline)
{
    // 基准：100 万条 2D 线段，对比两种「槽位/顶点分配顺序」，回答一个具体问题：
    //   几何仓里顶点怎么摆，才能让**部分可见**时也能合批？
    //   - scatter  ：槽位按随机顺序分配，顶点区间与可见集无关
    //   - clustered：槽位按空间格排序分配，视口覆盖的格 => 连续槽位段
    // 剔除效果看 culled/visible 数，合批效果看 drawCalls 数。
    //
    // 数字受构建配置影响明显（Debug 未优化），因此只做「防灾难」的宽松断言，
    // 结论看打印出的量级与相对关系。
    constexpr uint32_t kCount = 1000000;
    constexpr float kSpan = 10000.0f;
    constexpr uint32_t kFrames = 5;
    constexpr int kViewCount = 3;

    std::mt19937 rng(20260813u);
    std::uniform_real_distribution<float> coord(0.0f, kSpan - 10.0f);
    std::vector<float> xs(kCount);
    std::vector<float> ys(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        xs[i] = coord(rng);
        ys[i] = coord(rng);
    }

    const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
    const float views[kViewCount][4] = {
        { 0.0f, 0.0f, 1000.0f, 1000.0f },
        { 4000.0f, 4000.0f, 5000.0f, 5000.0f },
        { 0.0f, 0.0f, kSpan, kSpan },
    };
    const char* viewNames[kViewCount] = { "local", "pan", "full" };

    struct SceneResult
    {
        double buildMs = 0.0;
        uint32_t upsertFailures = 0;
        double avgMs[kViewCount] = {};
        uint32_t visible[kViewCount] = {};
        uint32_t drawCalls[kViewCount] = {};
    };

    auto runScene = [&](bool clustered, SceneResult& out) {
        // 分配顺序：clustered 时按空间格排序，使视口覆盖的格对应连续槽位段，
        // 从而让排序后的可见条目顶点区间也连续——这正是 canMerge 的前提。
        std::vector<uint32_t> order(kCount);
        for (uint32_t i = 0; i < kCount; ++i)
        {
            order[i] = i;
        }
        if (clustered)
        {
            // 必须与空间索引内部的 kGridBaseCellSize 一致（256）。
            // 若用更大的格（如 1000），一个索引格内会混入来自不同聚类格的条目，
            // 槽位在索引格内变得稀疏，顶点区间重新不连续 —— 合批照样失效。
            // 这里硬编码而不 include 内部头：rxIncremental.h 会带入 Render::RHI 的
            // 同名类型（Capabilities / FrameStats），与本文件的 using namespace
            // Render::RT 产生歧义，正是 unity build 曾踩过的坑。
            constexpr float kCell = 256.0f;
            auto cellKey = [&](uint32_t i) {
                const uint32_t cx = static_cast<uint32_t>(xs[i] / kCell);
                const uint32_t cy = static_cast<uint32_t>(ys[i] / kCell);
                return cy * 64u + cx;
            };
            std::stable_sort(order.begin(), order.end(),
                             [&](uint32_t a, uint32_t b) { return cellKey(a) < cellKey(b); });
        }

        DrawListDesc listDesc{};
        listDesc.initialCapacity = kCount;
        listDesc.enableCulling = 1;
        listDesc.enableMerging = 1;
        const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);
        const BufferHandle buffer =
            makeVertexBuffer(runtime, static_cast<uint64_t>(kCount) * 2u * stride);

        const auto buildStart = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < kCount; ++k)
        {
            const uint32_t src = order[k];
            const float x = xs[src];
            const float y = ys[src];
            const float box[4] = { x, y, x + 5.0f, y + 5.0f };
            const DrawCommand command =
                makeListCommand(buffer, k * 2u * stride, 2, 1, PrimitiveTopology::Lines);
            if (rxDrawListUpsert(runtime, list, k, &command, box) != RxResult::Ok)
            {
                out.upsertFailures += 1;
            }
        }
        out.buildMs = elapsedMs(buildStart);

        for (int c = 0; c < kViewCount; ++c)
        {
            // 预热：首帧会带上缓冲扩容、cache 冷等一次性开销，
            // 混进平均值会把「local 比 pan 慢」这种假象算成结论。
            constexpr uint32_t kWarmupFrames = 3;
            for (uint32_t f = 0; f < kWarmupFrames; ++f)
            {
                if (rxSessionBeginFrame(session) != RxResult::Ok)
                {
                    break;
                }
                rxSessionSubmitDrawList(session, list, views[c]);
                rxSessionEndFrame(session);
            }

            // 多轮取**最小值**而不是均值：环境噪声（其他进程、频率调节）
            // 只会让某一轮变慢，因此最小值最接近「无干扰」的真实耗时。
            // 用均值曾把噪声当成 15% 的优化收益（见设计文档 §14.1）。
            constexpr uint32_t kRounds = 5;
            double best = 0.0;
            for (uint32_t r = 0; r < kRounds; ++r)
            {
                const auto frameStart = std::chrono::steady_clock::now();
                for (uint32_t f = 0; f < kFrames; ++f)
                {
                    if (rxSessionBeginFrame(session) != RxResult::Ok)
                    {
                        break;
                    }
                    rxSessionSubmitDrawList(session, list, views[c]);
                    FrameStats frameStats{};
                    rxSessionGetStats(session, &frameStats);
                    out.visible[c] = kCount - frameStats.culledCommandCount;
                    out.drawCalls[c] = frameStats.drawCallCount;
                    rxSessionEndFrame(session);
                }
                const double average = elapsedMs(frameStart) / kFrames;
                best = (r == 0) ? average : (std::min)(best, average);
            }
            out.avgMs[c] = best;
        }

        rxDrawListDestroy(runtime, list);
        rxBufferDestroy(runtime, buffer);
    };

    SceneResult scatter{};
    SceneResult clustered{};
    runScene(false, scatter);
    runScene(true, clustered);

    const SceneResult* results[2] = { &scatter, &clustered };
    const char* labels[2] = { "scatter", "clustered" };
    std::cout << "\n[bench] primitives=" << kCount << " frames=" << kFrames << "\n";
    for (int s = 0; s < 2; ++s)
    {
        std::cout << "[bench] " << labels[s] << ": build=" << results[s]->buildMs << " ms\n";
        for (int c = 0; c < kViewCount; ++c)
        {
            std::cout << "[bench]   " << viewNames[c] << ": avg=" << results[s]->avgMs[c]
                      << " ms/frame, visible=" << results[s]->visible[c]
                      << ", drawCalls=" << results[s]->drawCalls[c] << "\n";
        }
    }
    std::cout << std::flush;

    RecordProperty("primitives", static_cast<int>(kCount));
    RecordProperty("scatter_local_ms", scatter.avgMs[0]);
    RecordProperty("clustered_local_ms", clustered.avgMs[0]);
    RecordProperty("clustered_local_drawcalls", static_cast<int>(clustered.drawCalls[0]));

    EXPECT_EQ(scatter.upsertFailures, 0u);
    EXPECT_EQ(clustered.upsertFailures, 0u);

    // 剔除语义护栏：局部视图只看得到很小一部分，全图一条都不剔
    for (int s = 0; s < 2; ++s)
    {
        EXPECT_GT(results[s]->visible[0], 0u);
        EXPECT_LT(results[s]->visible[0], kCount / 10) << "局部视图应只看到很小一部分图元";
        EXPECT_EQ(results[s]->visible[2], kCount) << "全图视图必须一条都不剔";
    }

    // 核心结论：按空间聚集分配槽位后，部分可见也应显著合批
    EXPECT_LT(clustered.drawCalls[0] * 10u, clustered.visible[0])
        << "clustered 场景的 draw call 数应远小于可见条目数（合批生效）";

    // 防灾难阈值：空间索引失效（退化 O(n^2) 或每帧全量重建）会远超此值
    EXPECT_LT(scatter.avgMs[0], 500.0) << "局部视图每帧耗时异常";
    EXPECT_LT(clustered.avgMs[2], 5000.0) << "全图视图每帧耗时异常";
}

// ==================== zoom out 耗时分解（每个变体独立进程跑） ====================
//
// 跑法：**逐个 filter 分别运行**，让每个变体独占一个进程。
//
//   RenderxGLTests --gtest_filter=*ZoomOutBreakdownCullMerge*
//   RenderxGLTests --gtest_filter=*ZoomOutBreakdownNoCull*
//   RenderxGLTests --gtest_filter=*ZoomOutBreakdownNoMerge*
//   RenderxGLTests --gtest_filter=*ZoomOutBreakdownNeither*
//
// 为什么必须拆进程：同一进程里连跑多个变体时，累积分配（每个列表 20 万条
// Entry）会改变内存状态，实测同一场景的耗时差异可达 2.4 倍——上一轮那个
// 假的「15% 优化收益」就是这么来的（见设计文档 §14.1）。

namespace
{
    struct ZoomOutMeasurement
    {
        double msPerFrame = 0.0;
        uint32_t drawCalls = 0;
    };

    struct BenchPositions
    {
        std::vector<float> xs;
        std::vector<float> ys;
    };

    /// 固定种子生成图元位置，保证各变体跑的是同一份场景
    BenchPositions makeBenchPositions(uint32_t count)
    {
        constexpr float kSpan = 10000.0f;
        std::mt19937 rng(20260813u);
        std::uniform_real_distribution<float> coord(0.0f, kSpan - 10.0f);

        BenchPositions positions;
        positions.xs.resize(count);
        positions.ys.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            positions.xs[i] = coord(rng);
            positions.ys[i] = coord(rng);
        }
        return positions;
    }

    /// 全图（zoom out）单变体测量：20 万条全部可见，走退化回退的线性路径。
    /// 多轮取最小值，抑制环境噪声。
    ZoomOutMeasurement measureZoomOut(RuntimeHandle runtime, SessionHandle session,
                                      const BenchPositions& positions, uint8_t culling,
                                      uint8_t merging, const char* label)
    {
        constexpr uint32_t kFrames = 5;
        constexpr uint32_t kRounds = 7;
        constexpr float kSpan = 10000.0f;

        const uint32_t count = static_cast<uint32_t>(positions.xs.size());
        const uint32_t stride = rxVertexStride(VertexFormat::P3C3);
        const BufferHandle buffer =
            makeVertexBuffer(runtime, static_cast<uint64_t>(count) * 2u * stride);

        DrawListDesc listDesc{};
        listDesc.initialCapacity = count;
        listDesc.enableCulling = culling;
        listDesc.enableMerging = merging;
        const DrawListHandle list = rxDrawListCreate(runtime, &listDesc);

        for (uint32_t i = 0; i < count; ++i)
        {
            const float x = positions.xs[i];
            const float y = positions.ys[i];
            const float box[4] = { x, y, x + 5.0f, y + 5.0f };
            const DrawCommand command =
                makeListCommand(buffer, i * 2u * stride, 2, 1, PrimitiveTopology::Lines);
            rxDrawListUpsert(runtime, list, i, &command, box);
        }

        const float fullView[4] = { 0.0f, 0.0f, kSpan, kSpan };
        for (uint32_t f = 0; f < 3; ++f)
        {
            rxSessionBeginFrame(session);
            rxSessionSubmitDrawList(session, list, fullView);
            rxSessionEndFrame(session);
        }

        ZoomOutMeasurement result{};
        double best = 0.0;
        for (uint32_t r = 0; r < kRounds; ++r)
        {
            const auto start = std::chrono::steady_clock::now();
            for (uint32_t f = 0; f < kFrames; ++f)
            {
                rxSessionBeginFrame(session);
                rxSessionSubmitDrawList(session, list, fullView);
                FrameStats frameStats{};
                rxSessionGetStats(session, &frameStats);
                result.drawCalls = frameStats.drawCallCount;
                rxSessionEndFrame(session);
            }
            const double average = elapsedMs(start) / kFrames;
            best = (r == 0) ? average : (std::min)(best, average);
        }
        result.msPerFrame = best;

        std::cout << "[bench] " << label << ": " << best
                  << " ms/frame, drawCalls=" << result.drawCalls << "\n";
        std::cout << std::flush;

        rxDrawListDestroy(runtime, list);
        rxBufferDestroy(runtime, buffer);
        return result;
    }

    /// 规模取 20 万：够大到能体现 O(n) 行为，又不会让内存压力主导结果
    constexpr uint32_t kZoomOutCount = 200000;
}  // namespace

TEST_F(RxIncrementalFixture, ZoomOutBreakdownCullMerge)
{
    const BenchPositions positions = makeBenchPositions(kZoomOutCount);
    const ZoomOutMeasurement measured =
        measureZoomOut(runtime, session, positions, 1, 1, "cull+merge");
    EXPECT_GT(measured.drawCalls, 0u);
}

TEST_F(RxIncrementalFixture, ZoomOutBreakdownNoCull)
{
    // 关掉判交：与 CullMerge 的差值即判交成本
    const BenchPositions positions = makeBenchPositions(kZoomOutCount);
    const ZoomOutMeasurement measured =
        measureZoomOut(runtime, session, positions, 0, 1, "no-cull");
    EXPECT_GT(measured.drawCalls, 0u);
}

TEST_F(RxIncrementalFixture, ZoomOutBreakdownNoMerge)
{
    // 关掉合批：draw call 数应等于图元数
    const BenchPositions positions = makeBenchPositions(kZoomOutCount);
    const ZoomOutMeasurement measured =
        measureZoomOut(runtime, session, positions, 1, 0, "no-merge");
    EXPECT_EQ(measured.drawCalls, kZoomOutCount);
}

TEST_F(RxIncrementalFixture, ZoomOutBreakdownNeither)
{
    // 既不算也不合：纯遍历 + 输出成本基线
    const BenchPositions positions = makeBenchPositions(kZoomOutCount);
    const ZoomOutMeasurement measured =
        measureZoomOut(runtime, session, positions, 0, 0, "neither");
    EXPECT_EQ(measured.drawCalls, kZoomOutCount);
}

