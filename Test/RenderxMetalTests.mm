/**
 * @file RenderxMetalTests.mm
 * @brief Metal 后端的离屏渲染正确性测试（无窗口、无宿主）
 *
 * 为什么是 .mm：离屏渲染在 RT 层仍需要一个 ISurface（Session::beginFrame
 * 要求 surface 非空且属于该 device），而 Metal 的 createSurface 只接受
 * CocoaNsView（handleA = NSView*）。测试因此要直接创建一个不挂进窗口的
 * NSView 来承载 CAMetalLayer —— 这是 Objective-C 代码，只能写在 .mm 里。
 *
 * 为什么不需要窗口：离屏路径（rxSessionSetRenderTarget）不调用
 * acquireNextImage，因此不会碰 nextDrawable；NSView 是否在窗口里、
 * WindowSystem 是否可用都不影响本文件。
 *
 * 断言的是**像素结果**，而不是「有没有报错」。此前的用例（RhiCoreTests /
 * RxRuntimeTests）只覆盖了句柄生命周期与帧配对契约，即使 Metal 一条命令
 * 都没提交也全绿。本文件是 Metal 后端第一次被证明「真的画对了」。
 */

#include <gtest/gtest.h>

#include "render/renderx.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Render::RT;

namespace
{
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;

    /// 收集 DLL 日志：Metal 的失败路径都靠日志说明原因，断言里要能看到
    struct LogSink
    {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        /// 全部级别的日志（含 debug/info）。用于「管线键」这类需要日志
        /// 取证的断言：管线变体是按需创建的，只看像素结果无法区分
        /// 「复用了旧管线」与「按新格式补建了管线」。
        std::vector<std::string> lines;

        static void callback(LogLevel level, const char* message, void* userData)
        {
            auto* sink = static_cast<LogSink*>(userData);
            const std::string text = message ? message : "";
            sink->lines.push_back(text);
            if (level == LogLevel::Error)
            {
                sink->errors.push_back(text);
            }
            else if (level == LogLevel::Warn)
            {
                sink->warnings.push_back(text);
            }
        }

        std::string allErrors() const
        {
            std::string joined;
            for (const std::string& line : errors)
            {
                joined += line;
                joined += '\n';
            }
            return joined;
        }

        /// 失败时一并打印警告：DLL 的降级路径（如「3D 管线绑了但光照不可用，
        /// 网格将无光照」）只发 warn，只看 errors 会漏掉真正的成因。
        std::string allWarnings() const
        {
            std::string joined;
            for (const std::string& line : warnings)
            {
                joined += line;
                joined += '\n';
            }
            return joined;
        }
    };

    /// 屏幕空间 P3C3 顶点：像素坐标（左上原点）+ 顶点色
    struct ScreenP3C3Vertex
    {
        float x, y, z;
        float r, g, b;
    };
    static_assert(sizeof(ScreenP3C3Vertex) == 24, "P3C3 步长必须为 24");

    /// 屏幕空间 P2T2C4 顶点：像素坐标 + UV + 颜色调制
    struct ScreenP2T2C4Vertex
    {
        float x, y;
        float u, v;
        float r, g, b, a;
    };
    static_assert(sizeof(ScreenP2T2C4Vertex) == 32, "P2T2C4 步长必须为 32");

    /// 世界空间 P3T2C4 顶点：世界坐标 + UV + 颜色调制
    struct WorldP3T2C4Vertex
    {
        float x, y, z;
        float u, v;
        float r, g, b, a;
    };
    static_assert(sizeof(WorldP3T2C4Vertex) == 36, "P3T2C4 步长必须为 36");

    /// 世界空间 P3N3 顶点：位置 + 法线，24 字节（与 rxVertexStride(P3N3) 一致）
    struct WorldP3N3Vertex
    {
        float x, y, z;
        float nx, ny, nz;
    };
    static_assert(sizeof(WorldP3N3Vertex) == 24, "P3N3 步长必须为 24");

    /// 世界空间 P3C4 顶点：位置 + RGBA（2D 覆盖层的格式），28 字节
    struct WorldP3C4Vertex
    {
        float x, y, z;
        float r, g, b, a;
    };
    static_assert(sizeof(WorldP3C4Vertex) == 28, "P3C4 步长必须为 28");

    /// 某个矩形区域内的墨色统计（亮度超过阈值的像素视为「有字」）
    struct InkStats
    {
        uint32_t ink = 0;
        uint32_t total = 0;
        /// 墨色像素的通道累加值。
        ///
        /// 墨色 = 顶点色 × (alpha × 覆盖率)，同一个字形上覆盖率逐像素不同，
        /// 但它对各通道是**同一个因子**，因此累加值之间的比值恒等于顶点色的
        /// 通道比。用比值断言可以在不知道覆盖率的情况下同时锁住两件事：
        /// R8 图集是否被当成了颜色（那样 g/b 会恒为 0）、通道顺序是否被
        /// 交换（BGRA 管线绑到 RGBA 附件上）。
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
    };

    /// 离屏 Metal 渲染的最小装配：Runtime + NSView 表面 + Session + 颜色附件
    class MetalOffscreenFixture : public ::testing::Test
    {
    protected:
        LogSink sink;
        RuntimeHandle runtime = RuntimeHandle::Invalid;
        SurfaceHandle surface = SurfaceHandle::Invalid;
        SessionHandle session = SessionHandle::Invalid;
        TextureHandle colorTarget = TextureHandle::Invalid;
        FontHandle testFont = FontHandle::Invalid;
        NSView* view = nil;

        void SetUp() override
        {
            // 后端没编译进来 / 本机没有 Metal 设备时跳过，而不是失败：
            // 这条用例的职责是「画得对不对」，不是「有没有 GPU」。
            if (rxIsBackendAvailable(Backend::Metal) == 0)
            {
                GTEST_SKIP() << "Metal backend is not available in this build";
            }

            RuntimeDesc runtimeDesc{};
            runtimeDesc.abiVersion = RENDERX_ABI_VERSION;
            runtimeDesc.backend = Backend::Metal;
            runtimeDesc.transientBufferBytes = 1u << 20;
            runtimeDesc.logCallback = &LogSink::callback;
            runtimeDesc.logUserData = &sink;
            runtimeDesc.applicationName = "RenderxMetalTests";
            runtime = rxRuntimeCreate(&runtimeDesc);
            ASSERT_TRUE(rxValid(runtime)) << "rxRuntimeCreate(Metal) failed:\n" << sink.allErrors();

            view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kWidth, kHeight)];
            ASSERT_NE(view, nil);

            SurfaceDesc surfaceDesc{};
            surfaceDesc.windowKind = NativeWindowKind::CocoaNsView;
            surfaceDesc.handleA = (__bridge void*)view;
            surfaceDesc.presentMode = PresentMode::Fifo;
            surfaceDesc.width = kWidth;
            surfaceDesc.height = kHeight;
            surfaceDesc.enableDepth = 0;
            surface = rxSurfaceCreate(runtime, &surfaceDesc);
            ASSERT_TRUE(rxValid(surface)) << "Metal surface creation failed:\n" << sink.allErrors();

            SessionDesc sessionDesc{};
            sessionDesc.runtime = runtime;
            sessionDesc.surface = surface;
            sessionDesc.clearColor[0] = 0.0f;
            sessionDesc.clearColor[1] = 0.0f;
            sessionDesc.clearColor[2] = 0.0f;
            sessionDesc.clearColor[3] = 1.0f;
            session = rxSessionCreate(&sessionDesc);
            ASSERT_TRUE(rxValid(session));

            RenderTargetDesc targetDesc{};
            targetDesc.width = kWidth;
            targetDesc.height = kHeight;
            targetDesc.usage = TextureUsageFlag::ColorAttachment | TextureUsageFlag::TransferSrc;
            colorTarget = rxTextureCreateRenderTarget(runtime, &targetDesc);
            ASSERT_TRUE(rxValid(colorTarget)) << "offscreen color target creation failed";

            ASSERT_EQ(rxSessionSetRenderTarget(session, colorTarget, TextureHandle::Invalid, kWidth,
                                               kHeight),
                      RxResult::Ok);
        }

        void TearDown() override
        {
            // DLL 的失败路径只通过日志说明原因。测试失败时如果看不到这些
            // 日志，就只能靠猜——把它打到失败输出里。
            //
            // 只在失败时打印：内建管线里 ScreenGlyph / Mesh3D 等 5 条缺 shader
            // 的会在每个用例的 Runtime 创建期各报一条 error，那属于已知缺口
            // （M2 补字形、M5 补 3D），不该把每一条通过用例的输出都刷满。
            if (::testing::Test::HasFailure() && !sink.errors.empty())
            {
                std::fprintf(stderr, "[metal-dll-errors]\n%s\n", sink.allErrors().c_str());
            }
            if (::testing::Test::HasFailure() && !sink.warnings.empty())
            {
                std::fprintf(stderr, "[metal-dll-warnings]\n%s\n", sink.allWarnings().c_str());
            }
            std::fflush(stderr);

            if (rxValid(session))
            {
                rxSessionDestroy(session);
            }
            if (rxValid(testFont))
            {
                rxFontDestroy(runtime, testFont);
            }
            if (rxValid(colorTarget))
            {
                rxTextureDestroy(runtime, colorTarget);
            }
            if (rxValid(surface))
            {
                rxSurfaceDestroy(runtime, surface);
            }
            if (rxValid(runtime))
            {
                rxRuntimeDestroy(runtime);
            }
            view = nil;
        }

        /// 读回整张离屏颜色附件：RGBA8、左上原点、逐行紧凑
        std::vector<uint8_t> readback()
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0xAB);
            const RxResult read = rxSessionReadPixelsFromTexture(session, colorTarget, 0, 0, kWidth,
                                                                kHeight, pixels.data(), pixels.size());
            EXPECT_EQ(read, RxResult::Ok) << "readback failed: " << rxResultName(read);
            return pixels;
        }

        static const uint8_t* pixelAt(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y)
        {
            return pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
        }

        /// 断言某像素等于期望 RGBA。容差 2 覆盖 float→unorm8 的取整差异
        static void expectPixel(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y,
                                int r, int g, int b, int a, const char* what)
        {
            const uint8_t* px = pixelAt(pixels, x, y);
            EXPECT_NEAR(px[0], r, 2) << what << " R at (" << x << "," << y << ")";
            EXPECT_NEAR(px[1], g, 2) << what << " G at (" << x << "," << y << ")";
            EXPECT_NEAR(px[2], b, 2) << what << " B at (" << x << "," << y << ")";
            EXPECT_NEAR(px[3], a, 2) << what << " A at (" << x << "," << y << ")";
        }

        /// 提交只含一条 DrawCommand 的绘制，viewport 取整张离屏目标
        static RxResult submitSingleCommand(SessionHandle handle, DrawCommand& command)
        {
            DrawPacket packet{};
            packet.commands = &command;
            packet.commandCount = 1;
            packet.enableCulling = 0;
            packet.viewMatrix[0] = 1.0f;
            packet.viewMatrix[5] = 1.0f;
            packet.viewMatrix[10] = 1.0f;
            packet.viewMatrix[15] = 1.0f;
            packet.viewport[0] = 0.0f;
            packet.viewport[1] = 0.0f;
            packet.viewport[2] = static_cast<float>(kWidth);
            packet.viewport[3] = static_cast<float>(kHeight);
            return rxSessionSubmit(handle, &packet);
        }

        /**
         * 在离屏目标上画一个盖住左半边的屏幕空间三角形。
         *
         * 顶点（像素，左上原点）：(0,0) (32,0) (0,64)，斜边为
         * x/32 + y/64 = 1，因此内部点满足 x/32 + y/64 < 1。
         */
        void drawLeftHalfTriangle(float r, float g, float b)
        {
            ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

            TransientAlloc alloc{};
            ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3C3) * 3, &alloc),
                      RxResult::Ok);
            auto* vertices = static_cast<ScreenP3C3Vertex*>(alloc.cpuPtr);
            vertices[0] = { 0.0f, 0.0f, 0.0f, r, g, b };
            vertices[1] = { 32.0f, 0.0f, 0.0f, r, g, b };
            vertices[2] = { 0.0f, 64.0f, 0.0f, r, g, b };

            DrawCommand command{};
            command.vertexBuffer = alloc.buffer;
            command.vertexOffset = alloc.offset;
            command.vertexCount = 3;
            command.topology = PrimitiveTopology::Triangles;
            command.space = RenderSpace::Screen;
            command.vertexFormat = VertexFormat::P3C3;
            command.indexType = IndexType::None;
            command.sortKey = rxMakeSortKey(10, 0, 0, 0);

            ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
            ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);
        }

        /**
         * 从测试字体文件建一个字体。
         *
         * 走真实的 rxFontCreate 路径（DLL 自己不做文件 IO，测试可以），
         * 因此图集纹理、懒光栅化、脏区上传都按生产逻辑走。
         */
        FontHandle loadTestFont(float pixelHeight, uint32_t sdfPadding)
        {
            std::ifstream file(RENDERX_TEST_FONT_PATH, std::ios::binary);
            if (!file.good())
            {
                ADD_FAILURE() << "缺少测试字体：" << RENDERX_TEST_FONT_PATH;
                return FontHandle::Invalid;
            }
            const std::vector<char> data((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());

            FontDesc desc{};
            desc.data = data.data();
            desc.dataBytes = data.size();
            desc.pixelHeight = pixelHeight;
            desc.sdfPadding = sdfPadding;

            FontHandle font = FontHandle::Invalid;
            const RxResult created = rxFontCreate(runtime, &desc, &font);
            if (created != RxResult::Ok)
            {
                ADD_FAILURE() << "rxFontCreate failed: " << rxResultName(created) << "\n"
                              << sink.allErrors();
                return FontHandle::Invalid;
            }
            return font;
        }

        /// 统计某矩形内的墨色。亮度阈值取 40/255，远高于清屏黑、远低于抗锯齿边缘
        static InkStats scanInk(const std::vector<uint8_t>& pixels, uint32_t x0, uint32_t y0,
                                uint32_t x1, uint32_t y1)
        {
            InkStats stats{};
            for (uint32_t y = y0; y < y1; ++y)
            {
                for (uint32_t x = x0; x < x1; ++x)
                {
                    const uint8_t* px = pixelAt(pixels, x, y);
                    const int luminance = (px[0] + px[1] + px[2]) / 3;
                    stats.total += 1;
                    if (luminance > 40)
                    {
                        stats.ink += 1;
                        stats.sumR += px[0];
                        stats.sumG += px[1];
                        stats.sumB += px[2];
                    }
                }
            }
            return stats;
        }

        /**
         * 断言墨色确实是「顶点色 × 覆盖率」。
         *
         * vertexRgb 是绘制时给的顶点色，其通道比必须原样出现在墨色的累加值上
         * ——覆盖率对三通道是同一因子，会被约掉。
         */
        void expectInkMatchesVertexColor(const InkStats& ink, float vertexR, float vertexG,
                                        float vertexB, const char* what)
        {
            ASSERT_GT(ink.ink, 0u) << what << "：一个墨色像素都没有";
            ASSERT_GT(ink.sumG, 0u) << what << "：绿通道累加为 0，R8 图集被当成了颜色";
            ASSERT_GT(ink.sumB, 0u) << what << "：蓝通道累加为 0，R8 图集被当成了颜色";

            // 以绿通道为基准比对通道比；容差 5% 覆盖 unorm8 量化与边缘像素
            const double rRatio = static_cast<double>(ink.sumR) / static_cast<double>(ink.sumG);
            const double bRatio = static_cast<double>(ink.sumB) / static_cast<double>(ink.sumG);
            const double expectedR = static_cast<double>(vertexR) / static_cast<double>(vertexG);
            const double expectedB = static_cast<double>(vertexB) / static_cast<double>(vertexG);

            EXPECT_NEAR(rRatio, expectedR, expectedR * 0.05)
                << what << "：红/绿比不符，通道顺序可能被交换（BGRA 管线绑到 RGBA 附件）";
            EXPECT_NEAR(bRatio, expectedB, expectedB * 0.05)
                << what << "：蓝/绿比不符，通道顺序可能被交换（BGRA 管线绑到 RGBA 附件）";
        }

        // ---- 3D 用例共用装配 ----

        /// 建一个「纯漫反射」材质：环境/高光系数留 0，让片元色只由方向光决定
        uint16_t addSolidMaterial(float r, float g, float b)
        {
            MaterialDesc mat{};
            mat.lineWidth = 1.0f;
            mat.pointSize = 1.0f;
            mat.color[0] = r;
            mat.color[1] = g;
            mat.color[2] = b;
            mat.color[3] = 1.0f;
            mat.flags = 0;
            mat.ambient[0] = mat.ambient[1] = mat.ambient[2] = 0.0f;
            mat.specular[0] = mat.specular[1] = mat.specular[2] = 0.0f;
            mat.shininess = 1.0f;
            const uint16_t index = rxMaterialAdd(runtime, &mat);
            EXPECT_NE(index, 0u) << "rxMaterialAdd failed:\n" << sink.allErrors();
            return index;
        }

        /// 单个白色方向光（默认沿 +Z，即位于法线 +Z 的面的正前方，Lambert 因子为 1），
        /// 关环境项、关高光、曝光 1、亮度下限 0：片元色恒等于材质漫反射色。
        /// 传入别的方向即可验证「direction 真的被读取」——若字段偏移错位，
        /// 切换方向不会改变画面。
        void applyKeyLight3D(float dirX = 0.0f, float dirY = 0.0f, float dirZ = 1.0f,
                             uint32_t doubleSided = 1)
        {
            Lighting3DDesc lighting{};
            lighting.ambientColor[0] = lighting.ambientColor[1] = lighting.ambientColor[2] = 0.0f;
            lighting.ambientEnabled = 0;
            lighting.ambientIntensity = 0.0f;
            lighting.doubleSided = doubleSided;
            lighting.specularEnabled = 0;
            lighting.specularIntensity = 0.0f;
            lighting.key.direction[0] = dirX;
            lighting.key.direction[1] = dirY;
            lighting.key.direction[2] = dirZ;  // 从表面指向光源
            lighting.key.enabled = 1;
            lighting.key.color[0] = lighting.key.color[1] = lighting.key.color[2] = 1.0f;
            lighting.key.intensity = 1.0f;
            lighting.viewPos[2] = 2.0f;
            lighting.minBrightness = 0.0f;
            lighting.exposure = 1.0f;
            rxSessionSetLighting3D(session, &lighting);  // 该接口按契约返回 void
        }

        /// 像素坐标 → NDC（左上原点、y 向下，与 screen_p3c3.vert 一致）
        static float ndcX(float px) { return px / static_cast<float>(kWidth) * 2.0f - 1.0f; }
        static float ndcY(float py) { return 1.0f - py / static_cast<float>(kHeight) * 2.0f; }

        /// 填一个面向 +Z 的逆时针四边形（两个三角形）。
        /// 绕序必须是逆时针：RasterState::frontFace 默认 CounterClockwise，
        /// 顺时针会被判成背面，doubleSided 据此把法线翻成背向光源的 (0,0,-1)，
        /// 光照随即归零（实测症状：整个网格全黑）。
        static void fillQuadCCW(WorldP3N3Vertex* v, float leftPx, float topPx, float rightPx,
                                float bottomPx, float z, float normalZ = 1.0f)
        {
            const float leftX = ndcX(leftPx);
            const float rightX = ndcX(rightPx);
            const float topY = ndcY(topPx);
            const float bottomY = ndcY(bottomPx);
            const float n[3] = { 0.0f, 0.0f, normalZ };
            v[0] = { leftX, bottomY, z, n[0], n[1], n[2] };
            v[1] = { rightX, bottomY, z, n[0], n[1], n[2] };
            v[2] = { rightX, topY, z, n[0], n[1], n[2] };
            v[3] = { leftX, bottomY, z, n[0], n[1], n[2] };
            v[4] = { rightX, topY, z, n[0], n[1], n[2] };
            v[5] = { leftX, topY, z, n[0], n[1], n[2] };
        }

        /// 填一个顺时针四边形（在该视角下被判为背面），法线朝 -Z。
        /// 用来模拟「导入网格法线朝外、绕序不可靠」：法线与可见面朝向相反，
        /// 只有 doubleSided 打开时片元才会翻转法线把它救回来。
        static void fillQuadCW(WorldP3N3Vertex* v, float leftPx, float topPx, float rightPx,
                               float bottomPx, float z)
        {
            const float leftX = ndcX(leftPx);
            const float rightX = ndcX(rightPx);
            const float topY = ndcY(topPx);
            const float bottomY = ndcY(bottomPx);
            const float n[3] = { 0.0f, 0.0f, -1.0f };
            v[0] = { leftX, bottomY, z, n[0], n[1], n[2] };
            v[1] = { rightX, topY, z, n[0], n[1], n[2] };
            v[2] = { rightX, bottomY, z, n[0], n[1], n[2] };
            v[3] = { leftX, bottomY, z, n[0], n[1], n[2] };
            v[4] = { leftX, topY, z, n[0], n[1], n[2] };
            v[5] = { rightX, topY, z, n[0], n[1], n[2] };
        }

        /// 建一个纯深度离屏附件（createRenderTargetTexture 对
        /// DepthStencilAttachment 用法固定给 D32Float）
        TextureHandle createDepthTarget()
        {
            RenderTargetDesc depthDesc{};
            depthDesc.width = kWidth;
            depthDesc.height = kHeight;
            depthDesc.usage = TextureUsageFlag::DepthStencilAttachment;
            const TextureHandle depth = rxTextureCreateRenderTarget(runtime, &depthDesc);
            EXPECT_TRUE(rxValid(depth)) << "offscreen depth target creation failed:\n"
                                        << sink.allErrors();
            return depth;
        }

        /// 取 from 之后新建管线的 depthFmt=（管线键的深度维度，见
        /// createPipelineFromKey 的 debug 日志）。只比较「两次是否不同」，
        /// 不依赖 Format 的具体枚举值。
        std::vector<int> depthFormatsCreatedSince(size_t from) const
        {
            std::vector<int> formats;
            for (size_t i = from; i < sink.lines.size(); ++i)
            {
                const std::string& line = sink.lines[i];
                const size_t at = line.find("depthFmt=");
                if (at != std::string::npos)
                {
                    formats.push_back(std::atoi(line.c_str() + at + 9));
                }
            }
            return formats;
        }

        /// 在一个覆盖 (8,8)-(56,56) 的四边形上画单个 P3N3 图元。
        /// backFacing=true 时改用顺时针绕序 + 朝 -Z 的法线（背面）。
        void drawSingleQuad(uint16_t materialIndex, uint16_t pipeline, float z, uint64_t sortKey,
                            bool backFacing = false)
        {
            ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

            TransientAlloc alloc{};
            ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3N3) * 6, &alloc),
                      RxResult::Ok);
            auto* vertices = static_cast<WorldP3N3Vertex*>(alloc.cpuPtr);
            if (backFacing)
            {
                fillQuadCW(vertices, 8.0f, 8.0f, 56.0f, 56.0f, z);
            }
            else
            {
                fillQuadCCW(vertices, 8.0f, 8.0f, 56.0f, 56.0f, z);
            }

            DrawCommand command{};
            command.vertexBuffer = alloc.buffer;
            command.vertexOffset = alloc.offset;
            command.vertexCount = 6;
            command.topology = PrimitiveTopology::Triangles;
            command.space = RenderSpace::World;
            command.vertexFormat = VertexFormat::P3N3;
            command.indexType = IndexType::None;
            command.pipelineIndex = pipeline;
            command.materialIndex = materialIndex;
            command.sortKey = sortKey;

            ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
            ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok) << sink.allErrors();
        }

        /// 用同一批顶点画「近红先提交、远蓝后提交」的两个重叠四边形（错误画家
        /// 顺序）。pipeline 决定深度测试开不开：开了近红胜出，关了后画的远蓝盖住红。
        /// 出参用引用而不是返回值，是为了能在这个 void 函数里用 ASSERT_* 中断。
        void drawOverlappingQuadsFarLast(uint16_t pipeline, uint16_t redMaterial,
                                        uint16_t blueMaterial, std::vector<uint8_t>& outPixels)
        {
            rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
            ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

            TransientAlloc alloc{};
            ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3N3) * 12, &alloc),
                      RxResult::Ok);
            auto* vertices = static_cast<WorldP3N3Vertex*>(alloc.cpuPtr);
            fillQuadCCW(vertices + 0, 24.0f, 24.0f, 40.0f, 40.0f, 0.3f);  // 近红
            fillQuadCCW(vertices + 6, 8.0f, 8.0f, 56.0f, 56.0f, 0.7f);    // 远蓝

            DrawCommand commands[2]{};
            for (DrawCommand& command : commands)
            {
                command.vertexBuffer = alloc.buffer;
                command.topology = PrimitiveTopology::Triangles;
                command.space = RenderSpace::World;
                command.vertexFormat = VertexFormat::P3N3;
                command.indexType = IndexType::None;
                command.vertexCount = 6;
                command.pipelineIndex = pipeline;
            }
            commands[0].vertexOffset = alloc.offset;
            commands[0].materialIndex = redMaterial;
            commands[0].sortKey = rxMakeSortKey(10, 0, 0, 0);  // 小键先画：近的反而先提交
            commands[1].vertexOffset = alloc.offset + rxVertexStride(VertexFormat::P3N3) * 6;
            commands[1].materialIndex = blueMaterial;
            commands[1].sortKey = rxMakeSortKey(20, 0, 0, 0);  // 大键后画：远的后画（错误顺序）

            DrawPacket packet{};
            packet.commands = commands;
            packet.commandCount = 2;
            packet.enableCulling = 0;
            packet.viewMatrix[0] = 1.0f;
            packet.viewMatrix[5] = 1.0f;
            packet.viewMatrix[10] = 1.0f;
            packet.viewMatrix[15] = 1.0f;
            packet.viewport[2] = static_cast<float>(kWidth);
            packet.viewport[3] = static_cast<float>(kHeight);
            ASSERT_EQ(rxSessionSubmit(session, &packet), RxResult::Ok);
            ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok) << sink.allErrors();

            outPixels = readback();
        }
    };
}  // namespace

// ==================== 能力位 ====================

TEST_F(MetalOffscreenFixture, MetalBackendIsAvailableAndReportsItself)
{
    EXPECT_EQ(rxIsBackendAvailable(Backend::Metal), 1u);

    Capabilities caps{};
    ASSERT_EQ(rxRuntimeGetCapabilities(runtime, &caps), RxResult::Ok);
    EXPECT_EQ(caps.backend, Backend::Metal);
    EXPECT_GT(caps.maxTextureSize, 0u);
    EXPECT_GE(caps.maxFramesInFlight, 1u);
    EXPECT_GT(std::string(caps.deviceName).size(), 0u);

    // 能力位必须与「实际可用的调用路径」一致：M3 已实现 compute / indirect
    EXPECT_EQ(caps.computeShaders, 1u);
    EXPECT_EQ(caps.indirectDraw, 1u);
    // M3 隔离（2025-09-15）：公共 ABI 的 Capabilities（renderx.h）暂无
    // multiDrawIndirect 字段（只有内部 RHI::Capabilities 有），本里程碑
    // ABI 零破坏不能新增，先移除该断言以恢复测试目标编译；待 multiDraw
    // 里程碑扩展公共 Capabilities 后由其恢复。
    // EXPECT_EQ(caps.multiDrawIndirect, 1u);
    // macOS 的 Metal 通过编码器状态 MTLTriangleFillModeLines 支持多边形线框
    // （不是管线描述符属性，见 MetalCommandList::bindPipeline）
    EXPECT_EQ(caps.wireframeFill, 1u);
}

// ==================== 清屏 ====================

TEST_F(MetalOffscreenFixture, ClearColorIsReadBackVerbatim)
{
    // 清屏是整附件操作：四角与中心都必须被覆盖，不受 viewport 影响
    rxSessionSetClearColor(session, 0.25f, 0.5f, 0.75f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    for (uint32_t y : { 0u, kHeight / 2, kHeight - 1 })
    {
        for (uint32_t x : { 0u, kWidth / 2, kWidth - 1 })
        {
            expectPixel(pixels, x, y, 64, 128, 191, 255, "clear color");
        }
    }
}

// ==================== 几何：屏幕空间三角形 ====================

TEST_F(MetalOffscreenFixture, ScreenSpaceTriangleCoversOnlyItsOwnArea)
{
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawLeftHalfTriangle(1.0f, 0.0f, 0.0f);

    const std::vector<uint8_t> pixels = readback();

    // 内部：x/32 + y/64 < 1
    expectPixel(pixels, 4, 8, 255, 0, 0, 255, "triangle interior");
    expectPixel(pixels, 4, 32, 255, 0, 0, 255, "triangle interior");

    // 外部：右半边与右下角必须仍是清屏色
    expectPixel(pixels, 56, 8, 0, 0, 0, 255, "outside triangle");
    expectPixel(pixels, 48, 48, 0, 0, 0, 255, "outside triangle");
    expectPixel(pixels, kWidth - 1, kHeight - 1, 0, 0, 0, 255, "outside triangle");
}

// ==================== 几何：屏幕空间带纹理四边形 ====================

TEST_F(MetalOffscreenFixture, ScreenSpaceTexturedQuadMultipliesVertexColor)
{
    constexpr uint32_t kTexSide = 4;
    std::vector<uint8_t> texels(static_cast<size_t>(kTexSide) * kTexSide * 4u);
    for (size_t i = 0; i < texels.size(); i += 4)
    {
        texels[i + 0] = 0;    // R
        texels[i + 1] = 255;  // G：纯绿纹理
        texels[i + 2] = 0;    // B
        texels[i + 3] = 255;  // A
    }

    TextureDesc textureDesc{};
    textureDesc.width = kTexSide;
    textureDesc.height = kTexSide;
    textureDesc.rgba = texels.data();
    textureDesc.rgbaBytes = texels.size();
    const TextureHandle texture = rxTextureCreate(runtime, &textureDesc);
    ASSERT_TRUE(rxValid(texture)) << "texture creation failed:\n" << sink.allErrors();

    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P2T2C4) * 6, &alloc),
              RxResult::Ok);
    auto* vertices = static_cast<ScreenP2T2C4Vertex*>(alloc.cpuPtr);
    // 铺满整张目标；顶点色为白，因此结果应等于纹理本身（见 screen_tex_p2t2c4.frag）
    const float w = static_cast<float>(kWidth);
    const float h = static_cast<float>(kHeight);
    vertices[0] = { 0.0f, 0.0f, 0.0f, 0.0f, 1, 1, 1, 1 };
    vertices[1] = { w, 0.0f, 1.0f, 0.0f, 1, 1, 1, 1 };
    vertices[2] = { w, h, 1.0f, 1.0f, 1, 1, 1, 1 };
    vertices[3] = { 0.0f, 0.0f, 0.0f, 0.0f, 1, 1, 1, 1 };
    vertices[4] = { w, h, 1.0f, 1.0f, 1, 1, 1, 1 };
    vertices[5] = { 0.0f, h, 0.0f, 1.0f, 1, 1, 1, 1 };

    DrawCommand command{};
    command.vertexBuffer = alloc.buffer;
    command.vertexOffset = alloc.offset;
    command.vertexCount = 6;
    command.texture = texture;
    command.topology = PrimitiveTopology::Triangles;
    command.space = RenderSpace::Screen;
    command.vertexFormat = VertexFormat::P2T2C4;
    command.indexType = IndexType::None;
    command.sortKey = rxMakeSortKey(10, 0, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    expectPixel(pixels, kWidth / 2, kHeight / 2, 0, 255, 0, 255, "textured quad center");
    expectPixel(pixels, 2, 2, 0, 255, 0, 255, "textured quad corner");

    rxTextureDestroy(runtime, texture);
}

// ==================== 帧间一致性 ====================

TEST_F(MetalOffscreenFixture, OffscreenFramesAreRepeatable)
{
    // 同样的输入连续两帧必须得到逐字节相同的结果。未提交的命令缓冲、
    // 上一帧的残留内容、写错的 loadOp（Load 而非 Clear）都会让这一条失败。
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawLeftHalfTriangle(0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> first = readback();

    drawLeftHalfTriangle(0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> second = readback();

    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size()), 0)
        << "离屏两帧结果不一致：说明帧间存在残留状态";

    // 顺带确认这一帧确实画出了东西（否则「两帧都全黑」也会通过上面的比较）
    expectPixel(first, 4, 8, 0, 255, 0, 255, "triangle interior");
    expectPixel(first, 56, 8, 0, 0, 0, 255, "outside triangle");
}

// ==================== 字形：R8 覆盖率（ScreenGlyph）====================

TEST_F(MetalOffscreenFixture, ScreenGlyphDrawsCoverageAsAlphaAndKeepsVertexColor)
{
    testFont = loadTestFont(32.0f, /*sdfPadding*/ 0);
    ASSERT_TRUE(rxValid(testFont));

    GlyphInfo glyph{};
    ASSERT_EQ(rxFontGlyph(runtime, testFont, static_cast<uint32_t>('H'), &glyph), RxResult::Ok);
    ASSERT_GT(glyph.width, 0.0f) << "字形位图为空，无法验证采样";
    ASSERT_GT(glyph.height, 0.0f);

    // 字形与位图同格式同拓扑，只有片元不同，必须由调用方显式指定管线；
    // 让 Runtime 自行解析会命中 ScreenTextured，把 R8 覆盖率当 RGBA 采样。
    const uint16_t pipeline = rxPipelineGetDefault(runtime, DefaultPipeline::ScreenGlyph);
    ASSERT_NE(pipeline, 0u) << "ScreenGlyph 管线未创建（缺 screen_glyph_p2t2c4_frag.metallib）";

    // 图集脏区必须在提交引用它的绘制之前上传
    ASSERT_EQ(rxFontFlushAtlas(runtime, testFont), RxResult::Ok);

    const float x0 = 4.0f;
    const float y0 = 4.0f;
    const float x1 = x0 + glyph.width;
    const float y1 = y0 + glyph.height;
    ASSERT_LE(x1, static_cast<float>(kWidth)) << "字形过大，装不进离屏目标";
    ASSERT_LE(y1, static_cast<float>(kHeight));

    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P2T2C4) * 6, &alloc),
              RxResult::Ok);
    auto* v = static_cast<ScreenP2T2C4Vertex*>(alloc.cpuPtr);
    // 顶点色刻意取非灰（0.25, 0.5, 1.0）：字形最终颜色是「顶点色 × 覆盖率」，
    // 用带比例的顶点色才能在像素上分辨通道是否被交换——纯白字做不到这一点。
    const float glyphColor[4] = { 0.25f, 0.5f, 1.0f, 1.0f };
    v[0] = { x0, y0, glyph.u0, glyph.v0, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };
    v[1] = { x1, y0, glyph.u1, glyph.v0, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };
    v[2] = { x1, y1, glyph.u1, glyph.v1, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };
    v[3] = { x0, y0, glyph.u0, glyph.v0, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };
    v[4] = { x1, y1, glyph.u1, glyph.v1, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };
    v[5] = { x0, y1, glyph.u0, glyph.v1, glyphColor[0], glyphColor[1], glyphColor[2], glyphColor[3] };

    DrawCommand command{};
    command.vertexBuffer = alloc.buffer;
    command.vertexOffset = alloc.offset;
    command.vertexCount = 6;
    command.topology = PrimitiveTopology::Triangles;
    command.space = RenderSpace::Screen;
    command.vertexFormat = VertexFormat::P2T2C4;
    command.indexType = IndexType::None;
    command.texture = rxFontAtlas(runtime, testFont);
    command.pipelineIndex = pipeline;
    command.sortKey = rxMakeSortKey(10, 0, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    const InkStats ink = scanInk(pixels, 0, 0, kWidth, kHeight);

    EXPECT_LT(ink.ink, ink.total) << "整个附件都成了墨色，不像是字形";
    // 墨色必须是「顶点色 × 覆盖率」：绿通道不得为 0（R8 图集当颜色的坑），
    // 通道比必须等于顶点色的通道比（通道顺序被交换的坑）
    expectInkMatchesVertexColor(ink, glyphColor[0], glyphColor[1], glyphColor[2], "ScreenGlyph");

    // 四边形之外必须仍是清屏色
    expectPixel(pixels, kWidth - 1, 0, 0, 0, 0, 255, "outside glyph quad");
    expectPixel(pixels, 0, kHeight - 1, 0, 0, 0, 255, "outside glyph quad");
}

// ==================== 字形：距离场（WorldGlyphSdf）====================

TEST_F(MetalOffscreenFixture, WorldGlyphSdfRendersSolidInteriorWithCorrectSign)
{
    // sdfPadding = 8：图集存距离场，字形位图四周各带 8 像素留白。
    // 留白区在轮廓之外，距离为负——这正是下面「边框不得有墨」的依据。
    testFont = loadTestFont(32.0f, /*sdfPadding*/ 8);
    ASSERT_TRUE(rxValid(testFont));

    GlyphInfo glyph{};
    ASSERT_EQ(rxFontGlyph(runtime, testFont, static_cast<uint32_t>('H'), &glyph), RxResult::Ok);
    ASSERT_GT(glyph.width, 0.0f) << "距离场位图为空，无法验证采样";
    ASSERT_GT(glyph.height, 0.0f);

    const uint16_t pipeline = rxPipelineGetDefault(runtime, DefaultPipeline::WorldGlyphSdf);
    ASSERT_NE(pipeline, 0u) << "WorldGlyphSdf 管线未创建（缺 world_glyph_sdf_p3t2c4_frag.metallib）";

    ASSERT_EQ(rxFontFlushAtlas(runtime, testFont), RxResult::Ok);

    // 世界空间：单位视图矩阵下顶点直接给 NDC。把字形铺在像素 (4,4) 起的矩形上，
    // 换算公式与 screen_p3c3.vert 一致（左上原点、y 向下）。
    const float px0 = 4.0f;
    const float py0 = 4.0f;
    const float px1 = px0 + glyph.width;
    const float py1 = py0 + glyph.height;
    ASSERT_LE(px1, static_cast<float>(kWidth)) << "字形过大，装不进离屏目标";
    ASSERT_LE(py1, static_cast<float>(kHeight));

    const auto ndcX = [](float px) { return px / static_cast<float>(kWidth) * 2.0f - 1.0f; };
    const auto ndcY = [](float py) { return 1.0f - py / static_cast<float>(kHeight) * 2.0f; };
    const float leftX = ndcX(px0);
    const float rightX = ndcX(px1);
    const float topY = ndcY(py0);
    const float bottomY = ndcY(py1);

    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3T2C4) * 6, &alloc),
              RxResult::Ok);
    auto* v = static_cast<WorldP3T2C4Vertex*>(alloc.cpuPtr);
    // 与覆盖率用例同一个非灰顶点色，理由见那里
    const float gR = 0.25f;
    const float gG = 0.5f;
    const float gB = 1.0f;
    v[0] = { leftX, topY, 0.0f, glyph.u0, glyph.v0, gR, gG, gB, 1.0f };
    v[1] = { rightX, topY, 0.0f, glyph.u1, glyph.v0, gR, gG, gB, 1.0f };
    v[2] = { rightX, bottomY, 0.0f, glyph.u1, glyph.v1, gR, gG, gB, 1.0f };
    v[3] = { leftX, topY, 0.0f, glyph.u0, glyph.v0, gR, gG, gB, 1.0f };
    v[4] = { rightX, bottomY, 0.0f, glyph.u1, glyph.v1, gR, gG, gB, 1.0f };
    v[5] = { leftX, bottomY, 0.0f, glyph.u0, glyph.v1, gR, gG, gB, 1.0f };

    DrawCommand command{};
    command.vertexBuffer = alloc.buffer;
    command.vertexOffset = alloc.offset;
    command.vertexCount = 6;
    command.topology = PrimitiveTopology::Triangles;
    command.space = RenderSpace::World;
    command.vertexFormat = VertexFormat::P3T2C4;
    command.indexType = IndexType::None;
    command.texture = rxFontAtlas(runtime, testFont);
    command.pipelineIndex = pipeline;
    // 与 WorldTextured 三元组相同，必须显式指定；否则距离场会被当 RGBA 采样
    command.sortKey = rxMakeSortKey(10, 0, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    const InkStats ink = scanInk(pixels, 0, 0, kWidth, kHeight);

    EXPECT_LT(ink.ink, ink.total) << "整个附件都成了墨色，不像是字形";
    expectInkMatchesVertexColor(ink, gR, gG, gB, "WorldGlyphSdf");

    // 符号正确性：留白区在轮廓之外，距离为负 → alpha 必为 0。
    // 若写成 0.5 - r（符号反相），字形会被反相，整圈边框反而全是墨。
    const uint32_t qx0 = static_cast<uint32_t>(px0);
    const uint32_t qy0 = static_cast<uint32_t>(py0);
    const uint32_t qx1 = static_cast<uint32_t>(px1);
    const uint32_t qy1 = static_cast<uint32_t>(py1);
    const uint32_t borderInk = scanInk(pixels, qx0, qy0, qx1, qy0 + 2).ink +
                               scanInk(pixels, qx0, qy1 - 2, qx1, qy1).ink +
                               scanInk(pixels, qx0, qy0, qx0 + 2, qy1).ink +
                               scanInk(pixels, qx1 - 2, qy0, qx1, qy1).ink;
    EXPECT_EQ(borderInk, 0u) << "距离场留白区出现墨色：SDF 的符号可能写反了";
}

// ==================== 2D 覆盖层：世界空间线段（P3C4 + Lines）====================

/**
 * 2D 视口「选中流水虚线」的等价装配：顶点格式 P3C4、空间 World、拓扑 Lines，
 * 且顶点 z 与 2D 视图矩阵一样是**直通**的（宿主 mat3ToMat4 把 out[10] 置 1，
 * world z 原样成为 NDC z），宿主给的 z 恒为 0。
 *
 * 这条用例存在的理由：产品实测「选中图元后本体消失、虚线轮廓不出现」，而运行
 * 日志显示宿主确实生成了覆盖层命令（overlay commands=8），并按需建出了
 * pipeline #26（vfmt=1 space=0 topo=1）。静态阅读 resolvePipeline 与
 * metalCommandList 都看不出问题，因此把这一组合单独钉在离屏像素上：
 * 失败 = Renderx 侧这条组合画不出来；通过 = 问题在宿主的几何数据。
 *
 * z 刻意取 0：Metal 的 NDC z 是 [0,1]（GL 是 [-1,1]），z=0 落在**近平面
 * 边界**上，是这条路径上最可疑的一个取值。
 */
TEST_F(MetalOffscreenFixture, WorldP3C4LineBatchIsVisible)
{
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    // 8 条横线，y 取像素行中心（1 - (2r+1)/64）：保证一定光栅化到整行像素，
    // 而不是恰好压在像素边界上。
    constexpr int kRows[8] = { 4, 12, 20, 28, 36, 44, 52, 60 };
    constexpr uint32_t kVertexCount = 16;
    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session,
                                      rxVertexStride(VertexFormat::P3C4) * kVertexCount, &alloc),
              RxResult::Ok);
    auto* v = static_cast<WorldP3C4Vertex*>(alloc.cpuPtr);
    const float lineR = 0.9f;
    const float lineG = 0.5f;
    const float lineB = 0.1f;
    const float lineA = 0.86f;
    for (int i = 0; i < 8; ++i)
    {
        const float y = 1.0f - (2.0f * static_cast<float>(kRows[i]) + 1.0f) / 64.0f;
        v[i * 2] = { -0.9f, y, 0.0f, lineR, lineG, lineB, lineA };
        v[i * 2 + 1] = { 0.9f, y, 0.0f, lineR, lineG, lineB, lineA };
    }

    DrawCommand command{};
    command.vertexBuffer = alloc.buffer;
    command.vertexOffset = alloc.offset;
    command.vertexCount = kVertexCount;
    command.topology = PrimitiveTopology::Lines;
    command.space = RenderSpace::World;
    command.vertexFormat = VertexFormat::P3C4;
    command.indexType = IndexType::None;
    // pipelineIndex 留 0：与 2D 覆盖层一致，由 Runtime 按 (格式, 空间, 拓扑) 解析
    command.sortKey = rxMakeSortKey(200, 1, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    const InkStats ink = scanInk(pixels, 0, 0, kWidth, kHeight);

    // 8 行 × 约 58 列 ≈ 464 个墨色像素；抗锯齿与端点差异用下限卡住
    EXPECT_GT(ink.ink, 300u) << "P3C4/World/Lines 一条线都没画出来\n"
                             << sink.allErrors() << sink.allWarnings();
    expectInkMatchesVertexColor(ink, lineR, lineG, lineB, "WorldP3C4Line");

    // 逐行核对：8 行中心必须都有墨，避免「只画出一行也算通过」
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t row = static_cast<uint32_t>(kRows[i]);
        EXPECT_GT(scanInk(pixels, 0, row, kWidth, row + 1).ink, 0u)
            << "第 " << kRows[i] << " 行为空";
    }
}

// ==================== 3D：深度遮挡（P3N3 + D32Float 离屏深度附件）====================

/**
 * 深度测试的「错误画家顺序」用例：
 * 近的红色小四边形 sortKey 更小（先画），远的蓝色大四边形 sortKey 更大
 * （后画）。没有深度缓冲时后画的蓝会盖满全屏；开启深度测试（Mesh3D
 * 默认 depthTest/write=1、LessEqual）后，蓝片在重叠区因 0.7 > 0.3 被丢弃，
 * 中心必须仍是红色；蓝片独有的外环区域保持蓝色，证明蓝片确实走完了
 * 光栅化、只是被深度拒绝——而不是整条管线没工作。
 *
 * 这同时验证了 Task 4 的管线键深度维度：内建 Mesh3D 管线按
 * （BGRA8、Unknown）预热，本用例的离屏目标是（RGBA8、D32Float），
 * pipelineWithFormats 必须在这里补建出双格式都匹配的变体，否则
 * MTL_DEBUG_LAYER 会在绑定/录制期报像素格式不匹配。
 */
TEST_F(MetalOffscreenFixture, Mesh3DDepthTestRejectsFarQuadDrawnAfterNearQuad)
{
    const TextureHandle depthTarget = createDepthTarget();
    ASSERT_TRUE(rxValid(depthTarget));
    ASSERT_EQ(rxSessionSetRenderTarget(session, colorTarget, depthTarget, kWidth, kHeight),
              RxResult::Ok);

    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t blueMaterial = addSolidMaterial(0.0f, 0.0f, 1.0f);
    applyKeyLight3D();

    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D 管线未创建（缺 mesh_3d_p3n3 metallib）";

    std::vector<uint8_t> pixels;
    drawOverlappingQuadsFarLast(meshPipeline, redMaterial, blueMaterial, pixels);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4u)
        << "读回失败，无法断言像素";

    // 重叠区：后画的远蓝必须被深度缓冲拒绝，中心是近红
    expectPixel(pixels, 32, 32, 255, 0, 0, 255, "near red wins over later far blue");
    expectPixel(pixels, 26, 26, 255, 0, 0, 255, "near red overlap edge");
    expectPixel(pixels, 38, 38, 255, 0, 0, 255, "near red overlap edge");
    // 蓝片独有外环：蓝片确实被光栅化，只是在重叠区被深度丢弃
    expectPixel(pixels, 12, 12, 0, 0, 255, 255, "far blue visible outside red quad");
    expectPixel(pixels, 52, 52, 0, 0, 255, 255, "far blue visible outside red quad");
    // 两片都没覆盖的角落仍是清屏黑
    expectPixel(pixels, 2, 2, 0, 0, 0, 255, "background untouched");
    expectPixel(pixels, 62, 62, 0, 0, 0, 255, "background untouched");

    // 恢复无深度离屏目标后再销毁深度纹理（帧外操作，见 setRenderTarget 契约）
    EXPECT_EQ(rxSessionSetRenderTarget(session, colorTarget, TextureHandle::Invalid, kWidth,
                                       kHeight),
              RxResult::Ok);
    rxTextureDestroy(runtime, depthTarget);
}

/**
 * 上一条用例的对照组：同样的几何、同样的提交顺序，但管线关掉深度测试
 * （自建 P3N3 管线，depthTest=0）。此时「后画的远蓝」按画家顺序盖住近红，
 * 中心变成蓝色。
 *
 * 两条用例合起来才构成完整归因：单看「中心的红赢了」无法区分「深度测试
 * 生效」与「第二个四边形压根没画」；这里证明蓝片确实能盖住红片，说明
 * 上一条用例的红是深度测试拦下来的，而不是蓝片丢失。
 */
TEST_F(MetalOffscreenFixture, Mesh3DWithoutDepthTestLetsLaterQuadWin)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t blueMaterial = addSolidMaterial(0.0f, 0.0f, 1.0f);
    applyKeyLight3D();

    // 自建一条「同格式、关深度测试」的 P3N3 管线（内建表里没有这种组合）
    PipelineDesc desc{};
    desc.topology = PrimitiveTopology::Triangles;
    desc.vertexFormat = VertexFormat::P3N3;
    desc.depthTest = 0;
    desc.depthWrite = 0;
    desc.blendEnable = 0;
    desc.depthFunc = DepthFunc::LessEqual;
    desc.fillMode = FillMode::Solid;
    desc.shaderName = nullptr;  // 按 vertexFormat + space 取默认 mesh_3d_p3n3
    const uint16_t noDepthPipeline = rxPipelineCreate(runtime, &desc);
    ASSERT_NE(noDepthPipeline, 0u) << "P3N3 无深度管线创建失败:\n" << sink.allErrors();

    std::vector<uint8_t> pixels;
    drawOverlappingQuadsFarLast(noDepthPipeline, redMaterial, blueMaterial, pixels);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4u)
        << "读回失败，无法断言像素";

    // 没有深度测试：后画的远蓝盖住近红（画家顺序生效）
    expectPixel(pixels, 32, 32, 0, 0, 255, 255, "later far blue covers near red (no depth test)");
    expectPixel(pixels, 26, 26, 0, 0, 255, 255, "later far blue covers near red (no depth test)");
    // 红片完全被盖住：它只覆盖 24..40，落在蓝片 8..56 之内
    expectPixel(pixels, 12, 12, 0, 0, 255, 255, "far blue outside red quad");
}

/**
 * 光照字段的端到端验证：同一个法线朝 +Z 的四边形，只切换主光方向。
 *
 * 光在 +Z（正面正前方）时 Lambert 因子为 1，片元色等于材质漫反射色；
 * 光转到 -Z（背后）时因子为 0，片元全黑。若 FrameUniforms 的字段偏移
 * 错位（例如 Task 3 修过的那类「读成邻字段」），切换 direction 不会改变
 * 画面——这条用例正是为此设的。
 */
TEST_F(MetalOffscreenFixture, Mesh3DLightingFollowsLightDirection)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D 管线未创建（缺 mesh_3d_p3n3 metallib）";

    // 光在正面：受光，片元色 = 材质色（红）
    applyKeyLight3D(0.0f, 0.0f, 1.0f);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> lit = readback();
    expectPixel(lit, 32, 32, 255, 0, 0, 255, "key light in front: surface lit");

    // 光转到正后方：dot(N, L) = -1，max(.,0) = 0 → 无光照贡献，全黑
    applyKeyLight3D(0.0f, 0.0f, -1.0f);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> unlit = readback();
    expectPixel(unlit, 32, 32, 0, 0, 0, 255, "key light behind: surface unlit");

    // 顺带确认两帧不是「都黑」（否则上面的失败信息会指向错误的方向）
    EXPECT_NE(std::memcmp(lit.data(), unlit.data(), lit.size()), 0)
        << "切换主光方向后画面完全没变：direction 可能没被着色器读到";
}

/**
 * 双面光照：模拟导入网格「法线朝外、绕序不可靠」的典型情况。
 *
 * 几何按顺时针提交（该视角下被判为背面），顶点法线给的是背向观察者的
 * (0,0,-1)，而主光在 +Z。doubleSided=1 时片元按 [[front_facing]] 翻转法线，
 * 法线变成 (0,0,1) 面向光源 → 受光；doubleSided=0 时不翻，法线与光相背
 * → Lambert 因子 0 → 全黑。这正是该开关存在的意义（OBJ/STL 绕序不一致时
 * 单面光照会让整片三角面变黑）。
 */
TEST_F(MetalOffscreenFixture, Mesh3DDoubleSidedLightsBackFacingQuad)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D 管线未创建（缺 mesh_3d_p3n3 metallib）";

    // 双面开启：背面法线翻转后受光
    applyKeyLight3D(0.0f, 0.0f, 1.0f, /*doubleSided*/ 1);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0),
                   /*backFacing*/ true);
    const std::vector<uint8_t> doubled = readback();
    expectPixel(doubled, 32, 32, 255, 0, 0, 255, "doubleSided=1: back face is lit");

    // 双面关闭：法线不翻，背面全黑（对照组，证明上一条的红来自翻转）
    applyKeyLight3D(0.0f, 0.0f, 1.0f, /*doubleSided*/ 0);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0),
                   /*backFacing*/ true);
    const std::vector<uint8_t> single = readback();
    expectPixel(single, 32, 32, 0, 0, 0, 255, "doubleSided=0: back face stays black");
}

/**
 * 深度格式必须进管线缓存键（Task 4 的核心）。
 *
 * 同一条内建 Mesh3D 管线先在「无深度附件」的离屏目标上画一次（键为
 * RGBA8/Unknown），再到「D32Float 深度附件」的目标上画一次（键必须变成
 * RGBA8/D32Float）。Metal 把 depthAttachmentPixelFormat 烘进管线对象，
 * 因此若深度格式没进键，第二次会复用第一条管线并被校验层判为像素格式
 * 不匹配——在 MTL_DEBUG_LAYER=1 下直接失败。两次都画出正确颜色，即证明
 * 深度维度确实按深度附件补建了变体。
 */
TEST_F(MetalOffscreenFixture, Mesh3DPipelineVariantIsKeyedByDepthFormat)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t blueMaterial = addSolidMaterial(0.0f, 0.0f, 1.0f);
    applyKeyLight3D();

    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D 管线未创建（缺 mesh_3d_p3n3 metallib）";

    // ---- 第一帧：SetUp 设的离屏目标没有深度附件 ----
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    const size_t logMarkWithoutDepth = sink.lines.size();
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    expectPixel(readback(), 32, 32, 255, 0, 0, 255, "no-depth target: quad is drawn");
    const std::vector<int> noDepthCreated = depthFormatsCreatedSince(logMarkWithoutDepth);
    ASSERT_EQ(noDepthCreated.size(), 1u)
        << "首帧应恰好补建一条管线（内建预热键的深度格式与实际目标不符）";
    const int noDepthFormat = noDepthCreated.front();

    // ---- 第二帧：换到带 D32Float 深度附件的目标，同一条内建管线 ----
    const TextureHandle depthTarget = createDepthTarget();
    ASSERT_TRUE(rxValid(depthTarget));
    ASSERT_EQ(rxSessionSetRenderTarget(session, colorTarget, depthTarget, kWidth, kHeight),
              RxResult::Ok);

    const size_t logMarkWithDepth = sink.lines.size();
    drawSingleQuad(blueMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    expectPixel(readback(), 32, 32, 0, 0, 255, 255, "depth target: quad is drawn");
    const std::vector<int> withDepthCreated = depthFormatsCreatedSince(logMarkWithDepth);
    ASSERT_EQ(withDepthCreated.size(), 1u) << "换到带深度附件的目标后必须再补建一条管线";
    // 核心断言（TR-4.1）：深度格式确实进了缓存键——否则第二帧会命中第一帧
    // 那条「无深度」的管线，校验层会报像素格式不匹配，而这里根本不会有新键。
    EXPECT_NE(withDepthCreated.front(), noDepthFormat)
        << "深度格式未进管线键：第二帧复用了无深度附件的管线";

    EXPECT_EQ(rxSessionSetRenderTarget(session, colorTarget, TextureHandle::Invalid, kWidth,
                                       kHeight),
              RxResult::Ok);
    rxTextureDestroy(runtime, depthTarget);
}

/**
 * 线框填充：Metal 的 triangleFillMode 是**编码器状态**而不是管线描述符属性
 * （与 Vulkan 的 VK_POLYGON_MODE_LINE 不同），所以它只能在绑定管线时下发。
 * 一旦少了这一步，管线照样建得出来、能力位也可以声明支持，但画面永远是
 * 实心——宿主侧看到的就是「切到线框没反应」。
 *
 * 归因必须用两帧对照，单看一帧无法区分「线框生效」与「整条管线没工作」：
 *   帧一 Mesh3D  实心：内部被涂满（证明几何、光照、材质注册都是通的）
 *   帧二 Mesh3DWire：同样的几何与材质，内部回到清屏黑，只剩边线
 * 再对边上一小块做「有墨」断言，确保帧二的内部变黑不是因为什么都没画。
 *
 * 采样点 (20,20)/(44,44) 到四条边与对角线的距离都超过 12px，不会被
 * 1px 线宽擦到——实心管线在这两点上必然亮，是干净的判别点。
 */
TEST_F(MetalOffscreenFixture, Mesh3DWireFillDrawsEdgesInsteadOfSolidArea)
{
    const uint16_t whiteMaterial = addSolidMaterial(1.0f, 1.0f, 1.0f);
    applyKeyLight3D();

    const uint16_t solidPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    const uint16_t wirePipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3DWire);
    ASSERT_NE(solidPipeline, 0u) << "Mesh3D 管线未创建（缺 mesh_3d_p3n3 metallib）";
    ASSERT_NE(wirePipeline, 0u) << "Mesh3DWire 管线未创建（缺 mesh_3d_p3n3 metallib）";

    // 阈值 24 对着清屏黑 (0,0,0) 定：受光后的白材质远高于它
    const auto isLit = [](const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
        const uint8_t* px = pixelAt(pixels, x, y);
        return static_cast<int>(px[0]) + static_cast<int>(px[1]) + static_cast<int>(px[2]) > 24;
    };
    const auto countLit = [&isLit](const std::vector<uint8_t>& pixels) {
        size_t n = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                if (isLit(pixels, x, y))
                {
                    ++n;
                }
            }
        }
        return n;
    };

    // ---- 帧一：实心对照。四边形覆盖 (8,8)-(56,56) ----
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawSingleQuad(whiteMaterial, solidPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> solid = readback();
    ASSERT_EQ(solid.size(), static_cast<size_t>(kWidth) * kHeight * 4u);
    ASSERT_TRUE(isLit(solid, 20, 20)) << "实心对照内部 (20,20) 未涂满：几何/光照链本身就不通";
    ASSERT_TRUE(isLit(solid, 44, 44)) << "实心对照内部 (44,44) 未涂满：几何/光照链本身就不通";
    const size_t solidLit = countLit(solid);

    // ---- 帧二：同几何、同材质，只换成 fillMode = Wireframe 的管线 ----
    drawSingleQuad(whiteMaterial, wirePipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> wire = readback();
    ASSERT_EQ(wire.size(), static_cast<size_t>(kWidth) * kHeight * 4u);

    EXPECT_FALSE(isLit(wire, 20, 20)) << "线框内部 (20,20) 仍被填充：triangleFillMode 未下发";
    EXPECT_FALSE(isLit(wire, 44, 44)) << "线框内部 (44,44) 仍被填充：triangleFillMode 未下发";

    // 左边框（x≈8）一带必须有墨，否则上面的「内部变黑」只说明什么都没画
    size_t edgeInk = 0;
    for (uint32_t y = 16; y < 48; ++y)
    {
        for (uint32_t x = 6; x <= 10; ++x)
        {
            if (isLit(wire, x, y))
            {
                ++edgeInk;
            }
        }
    }
    EXPECT_GT(edgeInk, 0u) << "左边框没有墨：线框什么都没画出来";

    // 面积量级：线框只剩 4 条边 + 1 条对角线，必须比实心小一个量级
    const size_t wireLit = countLit(wire);
    EXPECT_LT(wireLit, solidLit / 4)
        << "线框涂色 " << wireLit << " px，实心 " << solidLit << " px——没有拉开量级差";
}
