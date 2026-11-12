/**
 * @file GlFboCacheTests.cpp
 * @brief GlDevice FBO 缓存行为单测 —— 用打桩的 GLFuncs 在无 GPU 上下文下验证
 *
 * 覆盖（对应黑图/花屏的三个根因）：
 *  1. destroyTexture 同步清除引用该纹理的 FBO 缓存项（GL 名字会被驱动复用）；
 *  2. 缓存有容量上限（kMaxCachedFramebuffers），反复创建不同附件组合不会单调增长；
 *  3. 过期句柄（纹理已销毁）传入 framebufferFor 时返回 0 并记 error，
 *     而不是静默建出空附件 FBO。
 *
 * 构造 GlDevice 不需要真实 GL 上下文：GLFuncs 是纯函数指针表，
 * GlDevice 构造与被测路径上所有 GL 调用都经过该表，打桩即可。
 */
#include <gtest/gtest.h>

#include "rhi/gl/glDevice.h"
#include "rhi/rhiGpuDevice.h"

#include <vector>

// 用独立命名空间包裹本文件：unity build 下多个测试文件合并进同一 TU，
// Render::RHI 与 Render::RT 存在同名类型，文件级 using 会跨文件泄漏并产生歧义。
namespace RenderxFboCacheTestDetail
{
using namespace Render::RHI;

// glLoader.h 只兜底了 GL_FRAMEBUFFER_COMPLETE，未兜底 incomplete 系列
#ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
    #define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#endif
// 与 glLoader.h 一致：枚举未导出时自备兜底
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
    #define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

namespace GlFboCacheFixtureDetail
{
    /// 打桩 GL 状态：名字分配、删除计数、可控的完整性报告
    struct StubGlState
    {
        GLuint nextName = 1;
        int deletedTextures = 0;
        int deletedFramebuffers = 0;
        /// >0 时前 N 次 CheckFramebufferStatus 返回该值，之后恢复 COMPLETE
        int incompleteCountdown = 0;
        GLenum incompleteStatus = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        /// 当前绑定的帧缓冲，模拟宿主的 FBO 绑定状态
        GLuint boundFramebuffer = 0;

        static StubGlState& get()
        {
            static StubGlState s;
            return s;
        }

        void reset()
        {
            nextName = 1;
            deletedTextures = 0;
            deletedFramebuffers = 0;
            incompleteCountdown = 0;
            incompleteStatus = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
            boundFramebuffer = 0;
        }
    };

    void RENDER_GLAPI stubGenTextures(GLsizei n, GLuint* names)
    {
        auto& s = StubGlState::get();
        for (GLsizei i = 0; i < n; ++i)
        {
            names[i] = s.nextName++;
        }
    }

    void RENDER_GLAPI stubDeleteTextures(GLsizei n, const GLuint*)
    {
        StubGlState::get().deletedTextures += n;
    }

    void RENDER_GLAPI stubBindTexture(GLenum, GLuint) {}

    void RENDER_GLAPI stubTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {}

    void RENDER_GLAPI stubTexParameteri(GLenum, GLenum, GLint) {}

    void RENDER_GLAPI stubGenFramebuffers(GLsizei n, GLuint* names)
    {
        auto& s = StubGlState::get();
        for (GLsizei i = 0; i < n; ++i)
        {
            names[i] = s.nextName++;
        }
    }

    void RENDER_GLAPI stubDeleteFramebuffers(GLsizei n, const GLuint*)
    {
        StubGlState::get().deletedFramebuffers += n;
    }

    void RENDER_GLAPI stubBindFramebuffer(GLenum, GLuint name)
    {
        StubGlState::get().boundFramebuffer = name;
    }

    void RENDER_GLAPI stubGetIntegerv(GLenum pname, GLint* out)
    {
        if (!out)
        {
            return;
        }
        // 只有「当前绘制帧缓冲」需要真实取值，其余一律 0：本文件不依赖其它查询
        *out = (pname == GL_DRAW_FRAMEBUFFER_BINDING)
            ? static_cast<GLint>(StubGlState::get().boundFramebuffer)
            : 0;
    }

    void RENDER_GLAPI stubFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}

    GLenum RENDER_GLAPI stubCheckFramebufferStatus(GLenum)
    {
        auto& s = StubGlState::get();
        if (s.incompleteCountdown > 0)
        {
            --s.incompleteCountdown;
            return s.incompleteStatus;
        }
        return GL_FRAMEBUFFER_COMPLETE;
    }

    struct GlFboCacheFixture : public ::testing::Test
    {
        GLFuncs funcs{};
        DeviceDesc desc{};
        gl::GlDevice* device = nullptr;

        void SetUp() override
        {
            StubGlState::get().reset();

            funcs.GenTextures = &stubGenTextures;
            funcs.DeleteTextures = &stubDeleteTextures;
            funcs.BindTexture = &stubBindTexture;
            funcs.TexImage2D = &stubTexImage2D;
            funcs.TexParameteri = &stubTexParameteri;
            funcs.GenFramebuffers = &stubGenFramebuffers;
            funcs.DeleteFramebuffers = &stubDeleteFramebuffers;
            funcs.BindFramebuffer = &stubBindFramebuffer;
            funcs.FramebufferTexture2D = &stubFramebufferTexture2D;
            funcs.CheckFramebufferStatus = &stubCheckFramebufferStatus;
            funcs.GetIntegerv = &stubGetIntegerv;

            desc.backend = BackendKind::OpenGL;
            device = new gl::GlDevice(desc, funcs);
        }

        void TearDown() override
        {
            delete device;
            device = nullptr;
        }

        TextureHandle makeColorTexture()
        {
            TextureDesc td;
            td.width = 4;
            td.height = 4;
            td.format = Format::RGBA8Unorm;
            return device->createTexture(td);
        }

        RenderPassBeginDesc passFor(TextureHandle tex)
        {
            RenderPassBeginDesc d{};
            d.colorAttachmentCount = 1;
            d.colorAttachments[0].texture = tex;
            d.extent = Extent2D{ 4, 4 };
            return d;
        }
    };
}  // namespace GlFboCacheFixtureDetail

using namespace GlFboCacheFixtureDetail;

TEST_F(GlFboCacheFixture, FirstRequestCreatesAndCachesOneEntry)
{
    TextureHandle tex = makeColorTexture();
    ASSERT_TRUE(tex.valid());

    const GLuint fbo = device->framebufferFor(passFor(tex));
    EXPECT_NE(fbo, 0u);
    EXPECT_EQ(device->framebufferCacheSize(), 1u);
}

TEST_F(GlFboCacheFixture, SameAttachmentComboHitsCache)
{
    TextureHandle tex = makeColorTexture();
    const GLuint a = device->framebufferFor(passFor(tex));
    const GLuint b = device->framebufferFor(passFor(tex));

    ASSERT_NE(a, 0u);
    EXPECT_EQ(a, b) << "The same attachment combination must hit the cache and must not create a duplicate FBO";
    EXPECT_EQ(device->framebufferCacheSize(), 1u);
    EXPECT_EQ(StubGlState::get().deletedFramebuffers, 0);
}

TEST_F(GlFboCacheFixture, DestroyTexturePurgesReferencingCacheEntries)
{
    // 黑图根因回归：GL 名字会被驱动复用，删纹理不清 FBO 缓存的话，
    // 新纹理拿到同一数字名后会命中绑着旧附件的 FBO。
    TextureHandle tex = makeColorTexture();
    const GLuint fbo = device->framebufferFor(passFor(tex));
    ASSERT_NE(fbo, 0u);
    ASSERT_EQ(device->framebufferCacheSize(), 1u);

    device->destroyTexture(tex);

    EXPECT_EQ(device->framebufferCacheSize(), 0u) << "Destroying a texture must also purge cache entries that reference it";
    EXPECT_EQ(StubGlState::get().deletedFramebuffers, 1) << "The FBO object of a purged cache entry must be returned to the driver";
    EXPECT_EQ(StubGlState::get().deletedTextures, 1);
}

TEST_F(GlFboCacheFixture, UnreferencedCacheEntriesSurviveTextureDestroy)
{
    TextureHandle texA = makeColorTexture();
    TextureHandle texB = makeColorTexture();
    ASSERT_NE(device->framebufferFor(passFor(texA)), 0u);
    ASSERT_NE(device->framebufferFor(passFor(texB)), 0u);
    ASSERT_EQ(device->framebufferCacheSize(), 2u);

    device->destroyTexture(texA);

    EXPECT_EQ(device->framebufferCacheSize(), 1u) << "Only entries referencing the destroyed texture are purged; other cache entries are unaffected";
}

TEST_F(GlFboCacheFixture, CacheSizeIsBounded)
{
    // 截图/导出会对不同附件组合反复建 FBO：没有上限就单调增长
    std::vector<TextureHandle> textures;
    for (int i = 0; i < 12; ++i)
    {
        TextureHandle tex = makeColorTexture();
        ASSERT_TRUE(tex.valid());
        textures.push_back(tex);
        ASSERT_NE(device->framebufferFor(passFor(tex)), 0u) << "Attachment combination " << i << " failed to create";
    }

    EXPECT_EQ(device->framebufferCacheSize(), gl::GlDevice::kMaxCachedFramebuffers);
    // 12 - 8 = 4 条被淘汰，每条淘汰都 DeleteFramebuffers 一次
    EXPECT_EQ(StubGlState::get().deletedFramebuffers, 4);
}

TEST_F(GlFboCacheFixture, StaleColorAttachmentHandleReturnsZeroAndLogs)
{
    TextureHandle tex = makeColorTexture();
    ASSERT_NE(device->framebufferFor(passFor(tex)), 0u);
    ASSERT_EQ(device->framebufferCacheSize(), 1u);

    // 过期句柄：记录已销毁但 handle.valid() 仍为 true（世代校验在 ResourcePool）
    device->destroyTexture(tex);
    const GLuint fbo = device->framebufferFor(passFor(tex));

    EXPECT_EQ(fbo, 0u) << "A stale handle must return 0 and never silently build an FBO with empty attachments";
}

TEST_F(GlFboCacheFixture, IncompleteCachedFboIsDroppedAndRecreated)
{
    TextureHandle tex = makeColorTexture();
    const GLuint first = device->framebufferFor(passFor(tex));
    ASSERT_NE(first, 0u);

    // 下一次命中复查时报一次不完整 → 应删除旧条目并重建出新的 FBO
    StubGlState::get().incompleteCountdown = 1;
    const GLuint second = device->framebufferFor(passFor(tex));

    EXPECT_NE(second, 0u) << "An incomplete check should rebuild a usable FBO";
    EXPECT_NE(second, first) << "The old FBO was judged incomplete and deleted; the new entry must be a new object";
    EXPECT_EQ(device->framebufferCacheSize(), 1u);
    EXPECT_EQ(StubGlState::get().deletedFramebuffers, 1);
}

TEST_F(GlFboCacheFixture, EndRenderPassRestoresHostFramebuffer)
{
    // 花屏根因回归：离屏 pass 结束后必须还原宿主的帧缓冲绑定。
    // 宿主的默认帧缓冲是靠 acquireNextImage 捕获「当前绑定」得到的，未还原
    // 就会把离屏 FBO 当成窗口帧缓冲，后续合成全落在离屏纹理上（还在采样
    // 同一张纹理），窗口画面冻结成上一帧残留。
    TextureHandle tex = makeColorTexture();
    ASSERT_TRUE(tex.valid());

    const GLuint hostFbo = 42;
    StubGlState::get().boundFramebuffer = hostFbo;  // 模拟 QOpenGLWidget 绑定的 FBO

    gl::GlCommandList cmd(device);
    cmd.beginFrame(nullptr);

    // 首次进入：FBO 缓存未命中，framebufferFor 内部会临时绑定 0
    ASSERT_EQ(cmd.beginRenderPass(passFor(tex)), RhiResult::Ok);
    const GLuint firstPassFbo = StubGlState::get().boundFramebuffer;
    EXPECT_NE(firstPassFbo, hostFbo) << "An offscreen pass must bind its own FBO";
    EXPECT_NE(firstPassFbo, 0u) << "An offscreen pass must not draw to framebuffer 0";
    cmd.endRenderPass();
    EXPECT_EQ(StubGlState::get().boundFramebuffer, hostFbo) << "The host framebuffer must be restored after an offscreen pass";

    // 第二次进入：FBO 缓存命中
    ASSERT_EQ(cmd.beginRenderPass(passFor(tex)), RhiResult::Ok);
    EXPECT_EQ(StubGlState::get().boundFramebuffer, firstPassFbo);
    cmd.endRenderPass();
    EXPECT_EQ(StubGlState::get().boundFramebuffer, hostFbo) << "The host framebuffer must also be restored on a cache hit";
}

TEST_F(GlFboCacheFixture, DeviceDestructionReleasesCachedFramebuffers)
{
    TextureHandle tex = makeColorTexture();
    ASSERT_NE(device->framebufferFor(passFor(tex)), 0u);

    delete device;
    device = nullptr;

    EXPECT_EQ(StubGlState::get().deletedFramebuffers, 1) << "Cached FBOs must be returned to the driver when the device is destroyed";
}
}  // namespace RenderxFboCacheTestDetail