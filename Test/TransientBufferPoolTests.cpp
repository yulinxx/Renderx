/**
 * @file TransientBufferPoolTests.cpp
 * @brief TransientBufferPool unit and integration tests
 *
 * 测试目标：
 * - 验证初始化与关闭流程
 * - 验证 allocate 的对齐和偏移计算
 * - 验证 beginFrame 的帧轮换与 fallback 清理
 * - 验证主缓冲区溢出时的 fallback 行为
 * - 验证持久映射的 CPU 指针可直接写入
 */

#include <gtest/gtest.h>
#include "../src/core/transientBufferPool.h"
#include "../src/rhi/rhiDevice.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#endif

#include <cstring>
#include <vector>

// ------------------------------------------------------------------
// Minimal GL context helper for Windows (used to load OpenGL functions)
// ------------------------------------------------------------------
#ifdef _WIN32
class MinimalGLContext
{
    HWND m_hwnd = nullptr;
    HDC m_hdc = nullptr;
    HGLRC m_hglrc = nullptr;

public:
    bool create()
    {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "TransientBufferPoolTest";
        RegisterClassA(&wc);

        m_hwnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!m_hwnd)
        {
            return false;
        }

        m_hdc = GetDC(m_hwnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 24;

        int pf = ChoosePixelFormat(m_hdc, &pfd);
        if (!pf || !SetPixelFormat(m_hdc, pf, &pfd))
        {
            return false;
        }

        m_hglrc = wglCreateContext(m_hdc);
        if (!m_hglrc)
        {
            return false;
        }

        if (!wglMakeCurrent(m_hdc, m_hglrc))
        {
            return false;
        }

        return true;
    }

    void destroy()
    {
        if (m_hglrc)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(m_hglrc);
            m_hglrc = nullptr;
        }
        if (m_hdc && m_hwnd)
        {
            ReleaseDC(m_hwnd, m_hdc);
            m_hdc = nullptr;
        }
        if (m_hwnd)
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void* nativeHandle() const
    {
        return m_hglrc;
    }
};
#else
class MinimalGLContext
{
public:
    bool create()
    {
        return false;
    }

    void destroy() {}

    void* nativeHandle() const
    {
        return nullptr;
    }
};
#endif

// ------------------------------------------------------------------
// Test fixture providing a live GLDevice when possible
// ------------------------------------------------------------------
class TransientBufferPoolTest : public ::testing::Test
{
protected:
    MinimalGLContext m_glCtx;
    Render::RHI::IDevice* m_device = nullptr;
    Render::core::TransientBufferPool m_pool;

    void SetUp() override
    {
        if (!m_glCtx.create())
        {
            GTEST_SKIP() << "Failed to create minimal GL context";
        }

        m_device = Render::RHI::createGLDevice();
        if (!m_device)
        {
            GTEST_SKIP() << "Failed to create GL device";
        }

        // nativeWindow is not used by GLDevice::initialize except for storage,
        // but gl_loader_init needs an active WGL context on this thread.
        if (!m_device->initialize(m_glCtx.nativeHandle(), 1, 1))
        {
            delete m_device;
            m_device = nullptr;
            GTEST_SKIP() << "Failed to initialize GL device";
        }
    }

    void TearDown() override
    {
        m_pool.shutdown();
        if (m_device)
        {
            m_device->shutdown();
            delete m_device;
            m_device = nullptr;
        }
        m_glCtx.destroy();
    }
};

// ------------------------------------------------------------------
// Tests
// ------------------------------------------------------------------

// 验证基本初始化和关闭不会崩溃
TEST_F(TransientBufferPoolTest, InitializeShutdown_Succeeds)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;  // 1 MB
    constexpr uint32_t kFrameCount = 3;

    EXPECT_TRUE(m_pool.initialize(m_device, kBufferSize, kFrameCount));
    EXPECT_EQ(m_pool.bufferSize(), kBufferSize);
    EXPECT_EQ(m_pool.usedSize(), 0u);
    EXPECT_EQ(m_pool.fallbackCount(), 0u);

    m_pool.shutdown();
    EXPECT_EQ(m_pool.bufferSize(), 0u);
}

// 验证 allocate 返回有效的 CPU 指针和正确偏移
TEST_F(TransientBufferPoolTest, Allocate_ReturnsValidPointer)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    auto alloc = m_pool.allocate(256);
    ASSERT_NE(alloc.buffer, Render::RHI::NullHandle);
    ASSERT_NE(alloc.cpuPtr, nullptr);
    EXPECT_EQ(alloc.offset, 0u);
    EXPECT_EQ(alloc.size, 256u);

    // 验证 CPU 指针可直接写入
    std::memset(alloc.cpuPtr, 0xAB, static_cast<size_t>(alloc.size));
}

// 验证 allocate 的对齐行为
TEST_F(TransientBufferPoolTest, Allocate_RespectsAlignment)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    auto a1 = m_pool.allocate(100, 256);
    auto a2 = m_pool.allocate(100, 256);
    auto a3 = m_pool.allocate(100, 64);

    ASSERT_NE(a1.cpuPtr, nullptr);
    ASSERT_NE(a2.cpuPtr, nullptr);
    ASSERT_NE(a3.cpuPtr, nullptr);

    EXPECT_EQ(a1.offset, 0u);
    EXPECT_EQ(a2.offset, 256u);  // 对齐到 256
    // 256 + 100 = 356, align_up(356, 64) = 384
    EXPECT_EQ(a3.offset, 384u);
}

// 验证 beginFrame 轮换缓冲区并重置 used
TEST_F(TransientBufferPoolTest, BeginFrame_RotatesAndResets)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    m_pool.allocate(1024);
    EXPECT_EQ(m_pool.usedSize(), 1024u);

    m_pool.beginFrame();
    EXPECT_EQ(m_pool.usedSize(), 0u);

    auto alloc2 = m_pool.allocate(512);
    EXPECT_EQ(alloc2.offset, 0u);  // new frame, starts from 0
    EXPECT_EQ(m_pool.usedSize(), 512u);
}

// 验证多次 beginFrame 后缓冲区轮转不会泄露
TEST_F(TransientBufferPoolTest, BeginFrame_MultipleCycles)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    for (int i = 0; i < 10; ++i)
    {
        auto alloc = m_pool.allocate(4096);
        ASSERT_NE(alloc.cpuPtr, nullptr) << "Frame " << i;
        m_pool.beginFrame();
    }
    EXPECT_EQ(m_pool.fallbackCount(), 0u);
}

// 验证溢出时触发 fallback
TEST_F(TransientBufferPoolTest, Overflow_TriggersFallback)
{
    constexpr uint64_t kBufferSize = 4096;  // small buffer to trigger overflow quickly
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    // Fill the primary buffer
    auto alloc1 = m_pool.allocate(2048);
    ASSERT_NE(alloc1.cpuPtr, nullptr);

    auto alloc2 = m_pool.allocate(2048);
    ASSERT_NE(alloc2.cpuPtr, nullptr);

    // This should overflow and use fallback
    auto alloc3 = m_pool.allocate(1024);
    ASSERT_NE(alloc3.cpuPtr, nullptr);
    EXPECT_GE(m_pool.fallbackCount(), 1u);

    // Verify fallback buffer can be written to
    std::memset(alloc3.cpuPtr, 0xCD, static_cast<size_t>(alloc3.size));
}

// 验证 beginFrame 会清理上一轮的 fallback
TEST_F(TransientBufferPoolTest, BeginFrame_CleansFallbacks)
{
    constexpr uint64_t kBufferSize = 4096;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 2));  // 2 frames to cycle faster

    // Frame 0: cause fallback
    auto alloc1 = m_pool.allocate(8192);
    ASSERT_NE(alloc1.cpuPtr, nullptr);
    EXPECT_EQ(m_pool.fallbackCount(), 1u);

    // Move to frame 1
    m_pool.beginFrame();

    // Move to frame 0 again (fallback from first frame 0 should be freed now)
    m_pool.beginFrame();

    // Frame 0 again: cause another fallback
    auto alloc2 = m_pool.allocate(8192);
    ASSERT_NE(alloc2.cpuPtr, nullptr);

    // fallbackCount should be 2 (first one was destroyed, second one created)
    EXPECT_EQ(m_pool.fallbackCount(), 2u);
}

// 验证零大小分配返回空结果
TEST_F(TransientBufferPoolTest, Allocate_ZeroSize_ReturnsEmpty)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    auto alloc = m_pool.allocate(0);
    EXPECT_EQ(alloc.buffer, Render::RHI::NullHandle);
    EXPECT_EQ(alloc.cpuPtr, nullptr);
}

// 验证未初始化时调用接口不会崩溃
TEST(TransientBufferPoolLogicTest, Uninitialized_SafeNoop)
{
    Render::core::TransientBufferPool pool;

    // Should not crash
    pool.beginFrame();
    auto alloc = pool.allocate(1024);
    EXPECT_EQ(alloc.buffer, Render::RHI::NullHandle);
    EXPECT_EQ(pool.usedSize(), 0u);
    pool.shutdown();  // safe to call multiple times
}

// 验证无效初始化参数返回 false
TEST(TransientBufferPoolLogicTest, Initialize_InvalidArgs_Fails)
{
    Render::core::TransientBufferPool pool;
    EXPECT_FALSE(pool.initialize(nullptr, 1024, 3));
    EXPECT_FALSE(pool.initialize(nullptr, 0, 3));
    EXPECT_FALSE(pool.initialize(nullptr, 1024, 0));
}

// 验证交错 allocate() 和 beginFrame() 不会导致数据竞争或崩溃
TEST_F(TransientBufferPoolTest, Interleaved_AllocateBeginFrame)
{
    constexpr uint64_t kBufferSize = 4096;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        auto a1 = m_pool.allocate(1024);
        ASSERT_NE(a1.cpuPtr, nullptr);
        std::memset(a1.cpuPtr, 0xAA, static_cast<size_t>(a1.size));

        m_pool.beginFrame();

        auto a2 = m_pool.allocate(512);
        ASSERT_NE(a2.cpuPtr, nullptr);
        std::memset(a2.cpuPtr, 0xBB, static_cast<size_t>(a2.size));

        auto a3 = m_pool.allocate(512);
        ASSERT_NE(a3.cpuPtr, nullptr);
        std::memset(a3.cpuPtr, 0xCC, static_cast<size_t>(a3.size));

        m_pool.beginFrame();
    }
}

// 验证同一帧中多次触发 fallback
TEST_F(TransientBufferPoolTest, MultipleFallbacks_SameFrame)
{
    constexpr uint64_t kBufferSize = 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    // a1=512, a2=512 刚好填满主池(1024)，a3 超出触发 fallback
    auto a1 = m_pool.allocate(512);
    auto a2 = m_pool.allocate(512);
    auto a3 = m_pool.allocate(512);

    ASSERT_NE(a1.cpuPtr, nullptr);
    ASSERT_NE(a2.cpuPtr, nullptr);
    ASSERT_NE(a3.cpuPtr, nullptr);

    EXPECT_EQ(m_pool.fallbackCount(), 1u);  // 仅 a3 触发 fallback
}

// 验证超大对齐请求（如 4096 字节对齐）
TEST_F(TransientBufferPoolTest, Allocate_LargeAlignment)
{
    constexpr uint64_t kBufferSize = 1024 * 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    auto a1 = m_pool.allocate(64, 4096);
    auto a2 = m_pool.allocate(64, 4096);

    ASSERT_NE(a1.cpuPtr, nullptr);
    ASSERT_NE(a2.cpuPtr, nullptr);

    EXPECT_EQ(a1.offset, 0u);
    EXPECT_EQ(a2.offset, 4096u);  // 对齐到 4096
}

// 验证主池刚好够、刚好不够的情况
TEST_F(TransientBufferPoolTest, Allocate_ExactFitAndJustOver)
{
    constexpr uint64_t kBufferSize = 1024;
    ASSERT_TRUE(m_pool.initialize(m_device, kBufferSize, 3));

    // 刚好填满主池（默认对齐 256，所以 1024 刚好占满）
    auto a1 = m_pool.allocate(1024);
    ASSERT_NE(a1.cpuPtr, nullptr);
    EXPECT_EQ(a1.offset, 0u);

    // 再多分配 1 字节，应触发 fallback
    auto a2 = m_pool.allocate(1);
    ASSERT_NE(a2.cpuPtr, nullptr);
    EXPECT_GE(m_pool.fallbackCount(), 1u);
}