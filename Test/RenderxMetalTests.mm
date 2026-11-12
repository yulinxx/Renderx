/**
 * @file RenderxMetalTests.mm
 * @brief Offscreen rendering correctness tests for the Metal backend (no window, no host)
 *
 * Why .mm: offscreen rendering still needs an ISurface at the RT layer
 * (Session::beginFrame requires a non-null surface belonging to that device), and
 * Metal's createSurface only accepts a CocoaNsView (handleA = NSView*). The tests
 * therefore create an NSView that is not attached to a window to host a CAMetalLayer --
 * this is Objective-C code and must live in a .mm file.
 *
 * Why no window: the offscreen path (rxSessionSetRenderTarget) does not call
 * acquireNextImage, so it never touches nextDrawable; whether the NSView is in a window
 * and whether WindowSystem is available do not affect this file.
 *
 * The assertions check **pixel results**, not "did anything report an error". Earlier
 * suites (RhiCoreTests / RxRuntimeTests) only covered handle lifetimes and frame pairing,
 * and stayed green even if Metal submitted zero commands. This file is the first proof
 * that the Metal backend actually draws correctly.
 */

#include <gtest/gtest.h>

#include "render/renderx.h"
#include "rhi/metal/metalDevice.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
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

    /// Collects DLL logs: Metal failure paths explain themselves through logs, and
    /// assertions need to surface them.
    struct LogSink
    {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        /// All log levels (including debug/info). Used by assertions that need log
        /// evidence, such as the "pipeline key" checks: pipeline variants are created
        /// on demand, so pixel results alone cannot distinguish "reused the old
        /// pipeline" from "created a pipeline for the new format".
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

        /// Print warnings together with failures: DLL degradation paths (for example
        /// "3D pipeline bound but lighting is unavailable, meshes will be unlit") only
        /// emit a warning, so looking at errors alone misses the real cause.
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

    /// Screen-space P3C3 vertex: pixel coordinates (top-left origin) + vertex color
    struct ScreenP3C3Vertex
    {
        float x, y, z;
        float r, g, b;
    };
    static_assert(sizeof(ScreenP3C3Vertex) == 24, "P3C3 stride must be 24");

    /// Screen-space P2T2C4 vertex: pixel coordinates + UV + color modulation
    struct ScreenP2T2C4Vertex
    {
        float x, y;
        float u, v;
        float r, g, b, a;
    };
    static_assert(sizeof(ScreenP2T2C4Vertex) == 32, "P2T2C4 stride must be 32");

    /// World-space P3T2C4 vertex: world coordinates + UV + color modulation
    struct WorldP3T2C4Vertex
    {
        float x, y, z;
        float u, v;
        float r, g, b, a;
    };
    static_assert(sizeof(WorldP3T2C4Vertex) == 36, "P3T2C4 stride must be 36");

    /// World-space P3N3 vertex: position + normal, 24 bytes (matches rxVertexStride(P3N3))
    struct WorldP3N3Vertex
    {
        float x, y, z;
        float nx, ny, nz;
    };
    static_assert(sizeof(WorldP3N3Vertex) == 24, "P3N3 stride must be 24");

    /// World-space P3C4 vertex: position + RGBA (the 2D overlay format), 28 bytes
    struct WorldP3C4Vertex
    {
        float x, y, z;
        float r, g, b, a;
    };
    static_assert(sizeof(WorldP3C4Vertex) == 28, "P3C4 stride must be 28");

    /// Ink statistics inside a rectangle (pixels brighter than the threshold count as ink)
    struct InkStats
    {
        uint32_t ink = 0;
        uint32_t total = 0;
        /// Per-channel sums over ink pixels.
        ///
        /// Ink = vertex color x (alpha x coverage). Coverage varies per pixel across a
        /// glyph, but it is the **same factor** for all channels, so the ratios between
        /// the sums equal the channel ratios of the vertex color. Ratio assertions can
        /// therefore pin down two things without knowing coverage: whether the R8 atlas
        /// was treated as color (g/b would be constantly 0), and whether channel order
        /// was swapped (BGRA pipeline bound to an RGBA attachment).
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
    };

    /// Minimal offscreen Metal setup: Runtime + NSView surface + Session + color attachment
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
            // Skip instead of failing when the backend is not compiled in or this machine
            // has no Metal device: this test's job is "does it render correctly", not
            // "is there a GPU".
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
            // DLL failure paths only explain themselves through logs. If a test fails
            // without these logs, the only option is guessing -- print them to the
            // failure output.
            //
            // Only print on failure: built-in pipelines such as ScreenGlyph / Mesh3D
            // (5 of them miss shaders) each emit one error while every test creates its
            // Runtime, which is a known gap (M2 adds glyphs, M5 adds 3D); printing on
            // every passing test would flood the output.
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

        /// Read back the whole offscreen color attachment: RGBA8, top-left origin, tightly packed
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

        /// Assert that a pixel matches the expected RGBA. Tolerance 2 covers float->unorm8 rounding
        static void expectPixel(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y,
                                int r, int g, int b, int a, const char* what)
        {
            const uint8_t* px = pixelAt(pixels, x, y);
            EXPECT_NEAR(px[0], r, 2) << what << " R at (" << x << "," << y << ")";
            EXPECT_NEAR(px[1], g, 2) << what << " G at (" << x << "," << y << ")";
            EXPECT_NEAR(px[2], b, 2) << what << " B at (" << x << "," << y << ")";
            EXPECT_NEAR(px[3], a, 2) << what << " A at (" << x << "," << y << ")";
        }

        /// Submit a draw containing a single DrawCommand, with the viewport covering the whole target
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
         * Draw a screen-space triangle covering the left half of the offscreen target.
         *
         * Vertices (pixels, top-left origin): (0,0) (32,0) (0,64). The hypotenuse is
         * x/32 + y/64 = 1, so interior points satisfy x/32 + y/64 < 1.
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
         * Create a font from the test font file.
         *
         * Uses the real rxFontCreate path (the DLL does no file IO itself, the test can),
         * so atlas textures, lazy rasterization and dirty-region uploads all follow
         * production logic.
         */
        FontHandle loadTestFont(float pixelHeight, uint32_t sdfPadding)
        {
            std::ifstream file(RENDERX_TEST_FONT_PATH, std::ios::binary);
            if (!file.good())
            {
                ADD_FAILURE() << "Missing test font: " << RENDERX_TEST_FONT_PATH;
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

        /// Count ink inside a rectangle. The luminance threshold is 40/255, well above
        /// cleared black and well below anti-aliased edges.
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
         * Assert that ink really is "vertex color x coverage".
         *
         * vertexRgb is the vertex color supplied at draw time; its channel ratios must
         * appear unchanged in the ink sums -- coverage is the same factor for all three
         * channels and cancels out.
         */
        void expectInkMatchesVertexColor(const InkStats& ink, float vertexR, float vertexG,
                                        float vertexB, const char* what)
        {
            ASSERT_GT(ink.ink, 0u) << what << ": no ink pixels at all";
            ASSERT_GT(ink.sumG, 0u) << what << ": green channel sum is zero; the R8 atlas was treated as color";
            ASSERT_GT(ink.sumB, 0u) << what << ": blue channel sum is zero; the R8 atlas was treated as color";

            // Compare channel ratios against green; 5% tolerance covers unorm8 quantization
            // and edge pixels.
            const double rRatio = static_cast<double>(ink.sumR) / static_cast<double>(ink.sumG);
            const double bRatio = static_cast<double>(ink.sumB) / static_cast<double>(ink.sumG);
            const double expectedR = static_cast<double>(vertexR) / static_cast<double>(vertexG);
            const double expectedB = static_cast<double>(vertexB) / static_cast<double>(vertexG);

            EXPECT_NEAR(rRatio, expectedR, expectedR * 0.05)
                << what << ": red/green ratio mismatch; channel order may be swapped (BGRA pipeline bound to an RGBA attachment)";
            EXPECT_NEAR(bRatio, expectedB, expectedB * 0.05)
                << what << ": blue/green ratio mismatch; channel order may be swapped (BGRA pipeline bound to an RGBA attachment)";
        }

        // ---- Shared setup for 3D cases ----

        /// Create a "pure diffuse" material: ambient/specular terms are 0 so the fragment
        /// color is determined solely by the directional light.
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

        /// A single white directional light (default along +Z, i.e. directly in front of a
        /// face whose normal is +Z, Lambert factor 1), ambient off, specular off, exposure
        /// 1, minimum brightness 0: the fragment color equals the material diffuse color.
        /// Passing another direction verifies that "direction is really read" -- if the
        /// field offset is wrong, changing the direction will not change the image.
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
            lighting.key.direction[2] = dirZ;  // from surface toward light
            lighting.key.enabled = 1;
            lighting.key.color[0] = lighting.key.color[1] = lighting.key.color[2] = 1.0f;
            lighting.key.intensity = 1.0f;
            lighting.viewPos[2] = 2.0f;
            lighting.minBrightness = 0.0f;
            lighting.exposure = 1.0f;
            rxSessionSetLighting3D(session, &lighting);  // this API returns void by contract
        }

        /// Pixel coordinates -> NDC (top-left origin, y down, same as screen_p3c3.vert)
        static float ndcX(float px) { return px / static_cast<float>(kWidth) * 2.0f - 1.0f; }
        static float ndcY(float py) { return 1.0f - py / static_cast<float>(kHeight) * 2.0f; }

        /// Fill a counter-clockwise quad facing +Z (two triangles).
        /// Winding must be counter-clockwise: RasterState::frontFace defaults to
        /// CounterClockwise, and clockwise would be classified as back-facing; with
        /// doubleSided the normal is then flipped to (0,0,-1) facing away from the light,
        /// and lighting drops to zero (observed symptom: the whole mesh is black).
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

        /// Fill a clockwise quad (classified as back-facing from this viewpoint), normal -Z.
        /// Used to simulate "imported mesh with outward normals and unreliable winding":
        /// the normal points away from the visible face, and only doubleSided flips the
        /// normal in the fragment to rescue it.
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

        /// Create a depth-only offscreen attachment (createRenderTargetTexture fixes
        /// D32Float for DepthStencilAttachment usage).
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

        /// Depth formats of pipelines created after "from" (the depth dimension of the
        /// pipeline key, see the debug log in createPipelineFromKey). Only compares
        /// "are they different", not specific Format enum values.
        ///
        /// This evidence chain requires "one pipeline = one log line". rxRuntime.cpp used
        /// to throttle this line at 200ms per process (a static shared across Runtime
        /// instances), so if another fixture had just logged it, this test's entire
        /// record was swallowed and the test failed randomly depending on run order.
        /// That throttling has been removed on the production side; the cache itself
        /// already bounds the log volume.
        std::vector<int> depthFormatsCreatedSince(size_t from) const
        {
            return parseDepthFormats(from, sink.lines.size());
        }

        /// Depth format of the last pipeline-creation log before "to". Built-in pipelines
        /// are prewarmed with Unknown depth during Runtime creation; use that as the
        /// baseline instead of hard-coding Format enum values.
        /// Returns -1 when the evidence log is missing; callers must fail explicitly
        /// rather than continue comparing sizes.
        int lastDepthFormatBefore(size_t to) const
        {
            const auto formats = parseDepthFormats(0, std::min(to, sink.lines.size()));
            return formats.empty() ? -1 : formats.back();
        }

        std::vector<int> parseDepthFormats(size_t from, size_t to) const
        {
            std::vector<int> formats;
            for (size_t i = from; i < to; ++i)
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

        /// Draw a single P3N3 primitive on a quad covering (8,8)-(56,56).
        /// With backFacing=true, use clockwise winding and a normal pointing -Z (back face).
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

        /// Draw two overlapping quads with the same vertex batch ("near red submitted
        /// first, far blue submitted later" -- the wrong painter order). pipeline decides
        /// whether depth testing is on: with it, near red wins; without it, the later far
        /// blue covers red.
        /// Output is a reference rather than a return value so ASSERT_* can abort inside
        /// this void function.
        void drawOverlappingQuadsFarLast(uint16_t pipeline, uint16_t redMaterial,
                                        uint16_t blueMaterial, std::vector<uint8_t>& outPixels)
        {
            rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
            ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

            TransientAlloc alloc{};
            ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P3N3) * 12, &alloc),
                      RxResult::Ok);
            auto* vertices = static_cast<WorldP3N3Vertex*>(alloc.cpuPtr);
            fillQuadCCW(vertices + 0, 24.0f, 24.0f, 40.0f, 40.0f, 0.3f);  // near red
            fillQuadCCW(vertices + 6, 8.0f, 8.0f, 56.0f, 56.0f, 0.7f);    // far blue

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
            commands[0].sortKey = rxMakeSortKey(10, 0, 0, 0);  // small key first: near is submitted first
            commands[1].vertexOffset = alloc.offset + rxVertexStride(VertexFormat::P3N3) * 6;
            commands[1].materialIndex = blueMaterial;
            commands[1].sortKey = rxMakeSortKey(20, 0, 0, 0);  // large key later: far is drawn later (wrong order)

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

// ==================== Capabilities ====================

TEST_F(MetalOffscreenFixture, MetalBackendIsAvailableAndReportsItself)
{
    EXPECT_EQ(rxIsBackendAvailable(Backend::Metal), 1u);

    Capabilities caps{};
    ASSERT_EQ(rxRuntimeGetCapabilities(runtime, &caps), RxResult::Ok);
    EXPECT_EQ(caps.backend, Backend::Metal);
    EXPECT_GT(caps.maxTextureSize, 0u);
    EXPECT_GE(caps.maxFramesInFlight, 1u);
    EXPECT_GT(std::string(caps.deviceName).size(), 0u);

    // Capability bits must match the call paths that are actually available: M3 implements compute / indirect
    EXPECT_EQ(caps.computeShaders, 1u);
    EXPECT_EQ(caps.indirectDraw, 1u);
    // M3 isolation (2025-09-15): the public ABI Capabilities (renderx.h) has no
    // multiDrawIndirect field yet (only the internal RHI::Capabilities does). This
    // milestone cannot add one without breaking ABI, so the assertion is removed to
    // restore compilation of the test target; it will be restored when the multiDraw
    // milestone extends the public Capabilities.
    // EXPECT_EQ(caps.multiDrawIndirect, 1u);
    // macOS Metal supports polygon wireframe through encoder state MTLTriangleFillModeLines
    // (not a pipeline descriptor property, see MetalCommandList::bindPipeline)
    EXPECT_EQ(caps.wireframeFill, 1u);
}

// ==================== Clear ====================

TEST_F(MetalOffscreenFixture, ClearColorIsReadBackVerbatim)
{
    // Clearing covers the whole attachment: all four corners and the center must be
    // covered regardless of the viewport.
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

// ==================== Geometry: screen-space triangle ====================

TEST_F(MetalOffscreenFixture, ScreenSpaceTriangleCoversOnlyItsOwnArea)
{
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawLeftHalfTriangle(1.0f, 0.0f, 0.0f);

    const std::vector<uint8_t> pixels = readback();

    // Interior: x/32 + y/64 < 1
    expectPixel(pixels, 4, 8, 255, 0, 0, 255, "triangle interior");
    expectPixel(pixels, 4, 32, 255, 0, 0, 255, "triangle interior");

    // Exterior: the right half and bottom-right corner must still be the clear color
    expectPixel(pixels, 56, 8, 0, 0, 0, 255, "outside triangle");
    expectPixel(pixels, 48, 48, 0, 0, 0, 255, "outside triangle");
    expectPixel(pixels, kWidth - 1, kHeight - 1, 0, 0, 0, 255, "outside triangle");
}

// ==================== Geometry: screen-space textured quad ====================

TEST_F(MetalOffscreenFixture, ScreenSpaceTexturedQuadMultipliesVertexColor)
{
    constexpr uint32_t kTexSide = 4;
    std::vector<uint8_t> texels(static_cast<size_t>(kTexSide) * kTexSide * 4u);
    for (size_t i = 0; i < texels.size(); i += 4)
    {
        texels[i + 0] = 0;    // R
        texels[i + 1] = 255;  // G: pure green texture
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
    // Cover the whole target; vertex color is white, so the result should equal the
    // texture itself (see screen_tex_p2t2c4.frag).
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

// ==================== Frame-to-frame consistency ====================

TEST_F(MetalOffscreenFixture, OffscreenFramesAreRepeatable)
{
    // The same input on two consecutive frames must produce byte-identical results.
    // Unsubmitted command buffers, leftover content from the previous frame, or a wrong
    // loadOp (Load instead of Clear) all make this fail.
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawLeftHalfTriangle(0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> first = readback();

    drawLeftHalfTriangle(0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> second = readback();

    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size()), 0)
        << "Two offscreen frames differ: residual state exists between frames";

    // Also confirm this frame actually drew something (otherwise "both frames are black"
    // would pass the comparison above).
    expectPixel(first, 4, 8, 0, 255, 0, 255, "triangle interior");
    expectPixel(first, 56, 8, 0, 0, 0, 255, "outside triangle");
}

// ==================== Glyph: R8 coverage (ScreenGlyph) ====================

TEST_F(MetalOffscreenFixture, ScreenGlyphDrawsCoverageAsAlphaAndKeepsVertexColor)
{
    testFont = loadTestFont(32.0f, /*sdfPadding*/ 0);
    ASSERT_TRUE(rxValid(testFont));

    GlyphInfo glyph{};
    ASSERT_EQ(rxFontGlyph(runtime, testFont, static_cast<uint32_t>('H'), &glyph), RxResult::Ok);
    ASSERT_GT(glyph.width, 0.0f) << "Glyph bitmap is empty; sampling cannot be verified";
    ASSERT_GT(glyph.height, 0.0f);

    // Same bitmap format and topology as the glyph, only the fragment differs, so the
    // caller must select the pipeline explicitly; letting the Runtime resolve it would
    // hit ScreenTextured and sample R8 coverage as RGBA.
    const uint16_t pipeline = rxPipelineGetDefault(runtime, DefaultPipeline::ScreenGlyph);
    ASSERT_NE(pipeline, 0u) << "ScreenGlyph pipeline was not created (missing screen_glyph_p2t2c4_frag.metallib)";

    // The atlas dirty region must be uploaded before submitting a draw that references it
    ASSERT_EQ(rxFontFlushAtlas(runtime, testFont), RxResult::Ok);

    const float x0 = 4.0f;
    const float y0 = 4.0f;
    const float x1 = x0 + glyph.width;
    const float y1 = y0 + glyph.height;
    ASSERT_LE(x1, static_cast<float>(kWidth)) << "Glyph is too large for the offscreen target";
    ASSERT_LE(y1, static_cast<float>(kHeight));

    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    TransientAlloc alloc{};
    ASSERT_EQ(rxSessionAllocTransient(session, rxVertexStride(VertexFormat::P2T2C4) * 6, &alloc),
              RxResult::Ok);
    auto* v = static_cast<ScreenP2T2C4Vertex*>(alloc.cpuPtr);
    // Deliberately non-gray vertex color (0.25, 0.5, 1.0): the final glyph color is
    // "vertex color x coverage", and only a non-uniform vertex color reveals swapped
    // channels in pixels -- pure white text cannot.
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

    EXPECT_LT(ink.ink, ink.total) << "The whole attachment is inked; this does not look like a glyph";
    // Ink must be "vertex color x coverage": green must not be 0 (the R8-atlas-as-color
    // trap), and channel ratios must match the vertex color ratios (the swapped-channel trap).
    expectInkMatchesVertexColor(ink, glyphColor[0], glyphColor[1], glyphColor[2], "ScreenGlyph");

    // Outside the quad must still be the clear color
    expectPixel(pixels, kWidth - 1, 0, 0, 0, 0, 255, "outside glyph quad");
    expectPixel(pixels, 0, kHeight - 1, 0, 0, 0, 255, "outside glyph quad");
}

// ==================== Glyph: distance field (WorldGlyphSdf) ====================

TEST_F(MetalOffscreenFixture, WorldGlyphSdfRendersSolidInteriorWithCorrectSign)
{
    // sdfPadding = 8: the atlas stores a distance field, and the glyph bitmap has 8
    // pixels of padding on each side. The padding lies outside the outline with negative
    // distance -- this is the basis for "the border must contain no ink" below.
    testFont = loadTestFont(32.0f, /*sdfPadding*/ 8);
    ASSERT_TRUE(rxValid(testFont));

    GlyphInfo glyph{};
    ASSERT_EQ(rxFontGlyph(runtime, testFont, static_cast<uint32_t>('H'), &glyph), RxResult::Ok);
    ASSERT_GT(glyph.width, 0.0f) << "Distance-field bitmap is empty; sampling cannot be verified";
    ASSERT_GT(glyph.height, 0.0f);

    const uint16_t pipeline = rxPipelineGetDefault(runtime, DefaultPipeline::WorldGlyphSdf);
    ASSERT_NE(pipeline, 0u) << "WorldGlyphSdf pipeline was not created (missing world_glyph_sdf_p3t2c4_frag.metallib)";

    ASSERT_EQ(rxFontFlushAtlas(runtime, testFont), RxResult::Ok);

    // World space: with a unit view matrix, vertices are NDC directly. Lay the glyph on
    // a rectangle starting at pixel (4,4); the conversion matches screen_p3c3.vert
    // (top-left origin, y down).
    const float px0 = 4.0f;
    const float py0 = 4.0f;
    const float px1 = px0 + glyph.width;
    const float py1 = py0 + glyph.height;
    ASSERT_LE(px1, static_cast<float>(kWidth)) << "Glyph is too large for the offscreen target";
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
    // Same non-gray vertex color as the coverage case; see the reason there
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
    // Same triple as WorldTextured; must be specified explicitly, otherwise the distance
    // field is sampled as RGBA.
    command.sortKey = rxMakeSortKey(10, 0, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    const InkStats ink = scanInk(pixels, 0, 0, kWidth, kHeight);

    EXPECT_LT(ink.ink, ink.total) << "The whole attachment is inked; this does not look like a glyph";
    expectInkMatchesVertexColor(ink, gR, gG, gB, "WorldGlyphSdf");

    // Sign correctness: the padding lies outside the outline with negative distance ->
    // alpha must be 0. If written as 0.5 - r (inverted sign), the glyph inverts and the
    // whole border becomes ink.
    const uint32_t qx0 = static_cast<uint32_t>(px0);
    const uint32_t qy0 = static_cast<uint32_t>(py0);
    const uint32_t qx1 = static_cast<uint32_t>(px1);
    const uint32_t qy1 = static_cast<uint32_t>(py1);
    const uint32_t borderInk = scanInk(pixels, qx0, qy0, qx1, qy0 + 2).ink +
                               scanInk(pixels, qx0, qy1 - 2, qx1, qy1).ink +
                               scanInk(pixels, qx0, qy0, qx0 + 2, qy1).ink +
                               scanInk(pixels, qx1 - 2, qy0, qx1, qy1).ink;
    EXPECT_EQ(borderInk, 0u) << "Ink appeared in the SDF padding: the SDF sign may be inverted";
}

// ==================== 2D overlay: world-space lines (P3C4 + Lines) ====================

/**
 * Equivalent setup for the 2D viewport "selected polyline dashed stroke": vertex format
 * P3C4, space World, topology Lines, and vertex z passes through the same way as the 2D
 * view matrix (the host's mat3ToMat4 sets out[10] = 1, so world z becomes NDC z
 * unchanged); the host always supplies z = 0.
 *
 * Why this case exists: in the field, "after selecting an entity the body disappears and
 * the dashed outline never appears", while runtime logs showed the host did generate
 * overlay commands (overlay commands=8) and did create pipeline #26 on demand
 * (vfmt=1 space=0 topo=1). Static reading of resolvePipeline and metalCommandList found
 * nothing, so this combination is pinned down on offscreen pixels:
 * failure = Renderx cannot draw this combination; pass = the problem is in the host's
 * geometry data.
 *
 * z is deliberately 0: Metal NDC z is [0,1] (GL is [-1,1]), so z = 0 sits exactly on the
 * **near-plane boundary**, the most suspicious value along this path.
 */
TEST_F(MetalOffscreenFixture, WorldP3C4LineBatchIsVisible)
{
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_EQ(rxSessionBeginFrame(session), RxResult::Ok);

    // 8 horizontal lines, y at pixel-row centers (1 - (2r+1)/64): this guarantees they
    // rasterize onto whole rows instead of landing exactly on pixel boundaries.
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
    // pipelineIndex stays 0: same as the 2D overlay, the Runtime resolves it from
    // (format, space, topology).
    command.sortKey = rxMakeSortKey(200, 1, 0, 0);

    ASSERT_EQ(submitSingleCommand(session, command), RxResult::Ok);
    ASSERT_EQ(rxSessionEndFrame(session), RxResult::Ok);

    const std::vector<uint8_t> pixels = readback();
    const InkStats ink = scanInk(pixels, 0, 0, kWidth, kHeight);

    // 8 rows x about 58 columns ~= 464 ink pixels; the lower bound absorbs anti-aliasing
    // and endpoint differences.
    EXPECT_GT(ink.ink, 300u) << "P3C4/World/Lines drew nothing at all\n"
                             << sink.allErrors() << sink.allWarnings();
    expectInkMatchesVertexColor(ink, lineR, lineG, lineB, "WorldP3C4Line");

    // Check each row: all 8 row centers must have ink, so "only one row was drawn" does
    // not pass.
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t row = static_cast<uint32_t>(kRows[i]);
        EXPECT_GT(scanInk(pixels, 0, row, kWidth, row + 1).ink, 0u)
            << "Row " << kRows[i] << " is empty";
    }
}

// ==================== 3D: depth occlusion (P3N3 + D32Float offscreen depth attachment) ====================

/**
 * The "wrong painter order" case for depth testing:
 * the near red small quad has a smaller sortKey (drawn first), the far blue large quad
 * has a larger sortKey (drawn later). Without a depth buffer, the later blue covers the
 * whole screen; with depth testing on (Mesh3D defaults depthTest/write = 1, LessEqual),
 * blue fragments in the overlap region are rejected because 0.7 > 0.3 and the center must
 * stay red; the ring only covered by blue stays blue, proving blue really went through
 * rasterization and was only rejected by depth -- rather than the whole pipeline doing
 * nothing.
 *
 * This also verifies the depth dimension of the Task 4 pipeline key: the built-in Mesh3D
 * pipeline is prewarmed for (BGRA8, Unknown), while this test's offscreen target is
 * (RGBA8, D32Float); pipelineWithFormats must create a variant matching both formats
 * here, otherwise MTL_DEBUG_LAYER reports a pixel-format mismatch during binding or
 * encoding.
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
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D pipeline was not created (missing mesh_3d_p3n3 metallib)";

    std::vector<uint8_t> pixels;
    drawOverlappingQuadsFarLast(meshPipeline, redMaterial, blueMaterial, pixels);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4u)
        << "Readback failed; cannot assert pixels";

    // Overlap region: the later far blue must be rejected by the depth buffer; the center is near red
    expectPixel(pixels, 32, 32, 255, 0, 0, 255, "near red wins over later far blue");
    expectPixel(pixels, 26, 26, 255, 0, 0, 255, "near red overlap edge");
    expectPixel(pixels, 38, 38, 255, 0, 0, 255, "near red overlap edge");
    // Blue-only outer ring: blue really was rasterized, just rejected by depth in the overlap
    expectPixel(pixels, 12, 12, 0, 0, 255, 255, "far blue visible outside red quad");
    expectPixel(pixels, 52, 52, 0, 0, 255, 255, "far blue visible outside red quad");
    // Corners not covered by either quad are still cleared black
    expectPixel(pixels, 2, 2, 0, 0, 0, 255, "background untouched");
    expectPixel(pixels, 62, 62, 0, 0, 0, 255, "background untouched");

    // Restore the no-depth offscreen target before destroying the depth texture (an
    // out-of-frame operation, see the setRenderTarget contract)
    EXPECT_EQ(rxSessionSetRenderTarget(session, colorTarget, TextureHandle::Invalid, kWidth,
                                       kHeight),
              RxResult::Ok);
    rxTextureDestroy(runtime, depthTarget);
}

/**
 * Control group for the previous case: same geometry, same submission order, but the
 * pipeline disables depth testing (a custom P3N3 pipeline with depthTest = 0). The later
 * far blue now covers the near red in painter order, and the center turns blue.
 *
 * Only both cases together form a complete attribution: "red won at the center" alone
 * cannot distinguish "depth testing works" from "the second quad was never drawn"; this
 * case proves blue can indeed cover red, so the red in the previous case was stopped by
 * depth testing, not lost blue.
 */
TEST_F(MetalOffscreenFixture, Mesh3DWithoutDepthTestLetsLaterQuadWin)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t blueMaterial = addSolidMaterial(0.0f, 0.0f, 1.0f);
    applyKeyLight3D();

    // Create a custom "same format, depth test off" P3N3 pipeline (no such combination
    // exists in the built-in table).
    PipelineDesc desc{};
    desc.topology = PrimitiveTopology::Triangles;
    desc.vertexFormat = VertexFormat::P3N3;
    desc.depthTest = 0;
    desc.depthWrite = 0;
    desc.blendEnable = 0;
    desc.depthFunc = DepthFunc::LessEqual;
    desc.fillMode = FillMode::Solid;
    desc.shaderName = nullptr;  // use the default mesh_3d_p3n3 for vertexFormat + space
    const uint16_t noDepthPipeline = rxPipelineCreate(runtime, &desc);
    ASSERT_NE(noDepthPipeline, 0u) << "Failed to create the P3N3 no-depth pipeline:\n" << sink.allErrors();

    std::vector<uint8_t> pixels;
    drawOverlappingQuadsFarLast(noDepthPipeline, redMaterial, blueMaterial, pixels);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4u)
        << "Readback failed; cannot assert pixels";

    // No depth testing: the later far blue covers the near red (painter order applies)
    expectPixel(pixels, 32, 32, 0, 0, 255, 255, "later far blue covers near red (no depth test)");
    expectPixel(pixels, 26, 26, 0, 0, 255, 255, "later far blue covers near red (no depth test)");
    // The red quad is fully covered: it spans 24..40, inside the blue quad's 8..56
    expectPixel(pixels, 12, 12, 0, 0, 255, 255, "far blue outside red quad");
}

/**
 * End-to-end verification of lighting fields: the same quad with normal +Z, only the key
 * light direction changes.
 *
 * With the light along +Z (directly in front of the face), the Lambert factor is 1 and
 * the fragment color equals the material diffuse color; when the light turns to -Z
 * (behind), the factor is 0 and fragments are black. If FrameUniforms field offsets were
 * wrong (for example the "read the neighboring field" class of bugs fixed in Task 3),
 * changing direction would not change the image -- this case exists precisely for that.
 */
TEST_F(MetalOffscreenFixture, Mesh3DLightingFollowsLightDirection)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D pipeline was not created (missing mesh_3d_p3n3 metallib)";

    // Light in front: lit, fragment color = material color (red)
    applyKeyLight3D(0.0f, 0.0f, 1.0f);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> lit = readback();
    expectPixel(lit, 32, 32, 255, 0, 0, 255, "key light in front: surface lit");

    // Light directly behind: dot(N, L) = -1, max(.,0) = 0 -> no lighting contribution, all black
    applyKeyLight3D(0.0f, 0.0f, -1.0f);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> unlit = readback();
    expectPixel(unlit, 32, 32, 0, 0, 0, 255, "key light behind: surface unlit");

    // Also confirm the two frames are not "both black" (otherwise the failure message
    // above would point in the wrong direction)
    EXPECT_NE(std::memcmp(lit.data(), unlit.data(), lit.size()), 0)
        << "The image did not change at all after switching the key light direction: the shader may not read direction";
}

/**
 * Two-sided lighting: simulates the typical case of imported meshes with "outward normals
 * and unreliable winding".
 *
 * Geometry is submitted clockwise (classified as back-facing from this viewpoint), and
 * the vertex normal supplied is (0,0,-1) pointing away from the viewer, while the key
 * light is along +Z. With doubleSided=1 the fragment flips the normal via
 * [[front_facing]], so it becomes (0,0,1) facing the light -> lit; with doubleSided=0 it
 * is not flipped, the normal and light oppose each other -> Lambert factor 0 -> all
 * black. That is exactly why this switch exists (with inconsistent OBJ/STL winding,
 * single-sided lighting turns entire triangle fans black).
 */
TEST_F(MetalOffscreenFixture, Mesh3DDoubleSidedLightsBackFacingQuad)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D pipeline was not created (missing mesh_3d_p3n3 metallib)";

    // Double-sided on: the flipped back-face normal is lit
    applyKeyLight3D(0.0f, 0.0f, 1.0f, /*doubleSided*/ 1);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0),
                   /*backFacing*/ true);
    const std::vector<uint8_t> doubled = readback();
    expectPixel(doubled, 32, 32, 255, 0, 0, 255, "doubleSided=1: back face is lit");

    // Double-sided off: the normal is not flipped and the back face is black (control
    // group proving the red above came from the flip)
    applyKeyLight3D(0.0f, 0.0f, 1.0f, /*doubleSided*/ 0);
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0),
                   /*backFacing*/ true);
    const std::vector<uint8_t> single = readback();
    expectPixel(single, 32, 32, 0, 0, 0, 255, "doubleSided=0: back face stays black");
}

/**
 * The depth format must be part of the pipeline cache key (the core of Task 4).
 *
 * An earlier version asserted "first frame has no depth -> second frame D32Float, so one
 * variant must be created for each". After 5906c49, Session::beginFrame automatically
 * attaches a D32Float depth texture to **every** offscreen target (fixing offscreen
 * export clipping), so the depth format is already the same on both frames: the second
 * frame should hit the cache and create no new pipeline. The old assertion therefore
 * failed constantly, and that was not a product regression -- it could not be papered
 * over by loosening the assertion or skipping; it had to be rewritten for the new
 * semantics.
 *
 * The two evidence segments now:
 *   1. Built-in pipelines are prewarmed during Runtime creation with Unknown depth and
 *      the swapchain color format; the first offscreen frame uses RGBA8/D32Float and
 *      must create a variant whose **depth dimension has been rewritten**. If
 *      depthFormat were not in the key, the variant would keep Unknown depth and the
 *      Metal validation layer would report a depthAttachmentPixelFormat mismatch.
 *   2. After switching to an explicit D32Float depth attachment, the key is unchanged ->
 *      zero new pipelines (no variant leak), and the same variant must still draw blue
 *      correctly, proving it really works on a RenderPass with depth.
 *
 * The pure-logic part "depthFormat participates in equality/hash" is separately guarded
 * by PipelineKeyDepthFormatIsPartOfCacheKey in RxRuntimeTests.cpp, which does not depend
 * on GPU, logs or run order.
 */
TEST_F(MetalOffscreenFixture, Mesh3DPipelineVariantIsKeyedByDepthFormat)
{
    const uint16_t redMaterial = addSolidMaterial(1.0f, 0.0f, 0.0f);
    const uint16_t blueMaterial = addSolidMaterial(0.0f, 0.0f, 1.0f);
    applyKeyLight3D();

    const uint16_t meshPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    ASSERT_NE(meshPipeline, 0u) << "Mesh3D pipeline was not created (missing mesh_3d_p3n3 metallib)";

    // The depth format used by built-in pipeline prewarming during SetUp (Unknown), as
    // the baseline; do not hard-code enum values.
    const int prewarmDepthFormat = lastDepthFormatBefore(sink.lines.size());
    ASSERT_GE(prewarmDepthFormat, 0)
        << "No pipeline created log during prewarming; the evidence chain is broken (check whether rxRuntime.cpp logs are throttled)";

    // ---- First frame: offscreen target, beginFrame attaches a D32Float depth attachment automatically ----
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    const size_t logMarkAutoDepth = sink.lines.size();
    drawSingleQuad(redMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    expectPixel(readback(), 32, 32, 255, 0, 0, 255, "auto-depth target: quad is drawn");
    const std::vector<int> autoDepthCreated = depthFormatsCreatedSince(logMarkAutoDepth);
    ASSERT_EQ(autoDepthCreated.size(), 1u)
        << "The first offscreen frame should create exactly one pipeline (the prewarm key does not match the RGBA8/D32Float target)";
    // Core assertion (TR-4.1): the created variant must rewrite the key according to the
    // target depth format, not keep the prewarmed Unknown.
    EXPECT_NE(autoDepthCreated.front(), prewarmDepthFormat)
        << "Depth format is not part of the pipeline key: the offscreen variant still uses the prewarmed Unknown depth";

    // ---- Second frame: switch to an explicit D32Float depth attachment, same built-in pipeline ----
    const TextureHandle depthTarget = createDepthTarget();
    ASSERT_TRUE(rxValid(depthTarget));
    ASSERT_EQ(rxSessionSetRenderTarget(session, colorTarget, depthTarget, kWidth, kHeight),
              RxResult::Ok);

    const size_t logMarkExplicitDepth = sink.lines.size();
    drawSingleQuad(blueMaterial, meshPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    expectPixel(readback(), 32, 32, 0, 0, 255, 255, "explicit depth target: quad is drawn");
    // Automatic depth and explicit depth are both D32Float: the key is unchanged, so the
    // same variant should be hit instead of creating another one.
    const std::vector<int> explicitDepthCreated = depthFormatsCreatedSince(logMarkExplicitDepth);
    EXPECT_TRUE(explicitDepthCreated.empty())
        << "Created " << explicitDepthCreated.size() << " duplicate pipeline(s) for the same depth format; the cache key is unstable";

    EXPECT_EQ(rxSessionSetRenderTarget(session, colorTarget, TextureHandle::Invalid, kWidth,
                                       kHeight),
              RxResult::Ok);
    rxTextureDestroy(runtime, depthTarget);
}

/**
 * Wireframe fill: Metal's triangleFillMode is **encoder state**, not a pipeline descriptor
 * property (unlike Vulkan's VK_POLYGON_MODE_LINE), so it can only be issued when binding
 * the pipeline. Without that step the pipeline still builds and the capability bit may
 * still claim support, but the image stays solid -- what the host observes is "switching
 * to wireframe does nothing".
 *
 * Attribution requires two frames; a single frame cannot distinguish "wireframe works"
 * from "the whole pipeline does nothing":
 *   frame 1: Mesh3D solid -- the interior is filled (proving geometry, lighting and
 *            material registration all work)
 *   frame 2: Mesh3DWire -- same geometry and material, the interior returns to cleared
 *            black with only edges visible
 * Then assert "has ink" on a small strip of the border to ensure the interior turning
 * black in frame 2 is not because nothing was drawn.
 *
 * Sample points (20,20)/(44,44) are more than 12px from all four edges and the diagonal,
 * so a 1px line width cannot touch them -- the solid pipeline must be bright at these
 * points, making them clean discriminators.
 */
TEST_F(MetalOffscreenFixture, Mesh3DWireFillDrawsEdgesInsteadOfSolidArea)
{
    const uint16_t whiteMaterial = addSolidMaterial(1.0f, 1.0f, 1.0f);
    applyKeyLight3D();

    const uint16_t solidPipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3D);
    const uint16_t wirePipeline = rxPipelineGetDefault(runtime, DefaultPipeline::Mesh3DWire);
    ASSERT_NE(solidPipeline, 0u) << "Mesh3D pipeline was not created (missing mesh_3d_p3n3 metallib)";
    ASSERT_NE(wirePipeline, 0u) << "Mesh3DWire pipeline was not created (missing mesh_3d_p3n3 metallib)";

    // Threshold 24 is set against cleared black (0,0,0): a lit white material is far above it
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

    // ---- Frame 1: solid control. The quad covers (8,8)-(56,56) ----
    rxSessionSetClearColor(session, 0.0f, 0.0f, 0.0f, 1.0f);
    drawSingleQuad(whiteMaterial, solidPipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> solid = readback();
    ASSERT_EQ(solid.size(), static_cast<size_t>(kWidth) * kHeight * 4u);
    ASSERT_TRUE(isLit(solid, 20, 20)) << "Solid control interior (20,20) is not filled: the geometry/lighting chain itself is broken";
    ASSERT_TRUE(isLit(solid, 44, 44)) << "Solid control interior (44,44) is not filled: the geometry/lighting chain itself is broken";
    const size_t solidLit = countLit(solid);

    // ---- Frame 2: same geometry and material, only the fillMode = Wireframe pipeline changes ----
    drawSingleQuad(whiteMaterial, wirePipeline, 0.5f, rxMakeSortKey(10, 0, 0, 0));
    const std::vector<uint8_t> wire = readback();
    ASSERT_EQ(wire.size(), static_cast<size_t>(kWidth) * kHeight * 4u);

    EXPECT_FALSE(isLit(wire, 20, 20)) << "Wireframe interior (20,20) is still filled: triangleFillMode was not issued";
    EXPECT_FALSE(isLit(wire, 44, 44)) << "Wireframe interior (44,44) is still filled: triangleFillMode was not issued";

    // The left border (x ~= 8) must have ink, otherwise "interior turned black" above
    // only means nothing was drawn.
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
    EXPECT_GT(edgeInk, 0u) << "The left border has no ink: the wireframe drew nothing";

    // Magnitude: wireframe has only 4 edges + 1 diagonal, so it must be an order of
    // magnitude smaller than solid.
    const size_t wireLit = countLit(wire);
    EXPECT_LT(wireLit, solidLit / 4)
        << "Wireframe covers " << wireLit << " px and solid covers " << solidLit << " px -- no order-of-magnitude gap";
}

// ==================== in-flight wait timeout ====================

TEST(MetalSurface, InFlightWaitTimesOutInsteadOfBlockingForever)
{
    // Never signal: simulate a hung GPU / stuck driver. The wait must return false after
    // the timeout instead of blocking the UI thread forever like DISPATCH_TIME_FOREVER
    // used to.
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    const auto begin = std::chrono::steady_clock::now();
    const bool acquired = Render::RHI::metal::waitSemaphore(semaphore, 50);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

    EXPECT_FALSE(acquired) << "The semaphore was never signaled, but the wait succeeded";
    EXPECT_GE(elapsedMs, 40) << "The wait returned early; the timeout did not actually take effect (" << elapsedMs << "ms)";
    EXPECT_LT(elapsedMs, 2000) << "The wait degenerated into a long block (" << elapsedMs << "ms)";

    // A timeout must not consume the count: after signal, the same semaphore must be
    // immediately available.
    dispatch_semaphore_signal(semaphore);
    EXPECT_TRUE(Render::RHI::metal::waitSemaphore(semaphore, 50));
}
