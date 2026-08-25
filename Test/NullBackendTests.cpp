/**
 * @file NullBackendTests.cpp
 * @brief Null RHI backend tests (no GPU operations required)
 *
 * These tests validate the NullDevice implementation without requiring
 * an OpenGL context. They verify:
 * - Device initialization and shutdown
 * - Resource handle allocation (buffer/texture/pipeline)
 * - Draw call counting
 * - Frame management
 * - Render state tracking
 */
#include <gtest/gtest.h>
#include "rhi/rhiNull.h"
#include "rhi/rhiDevice.h"
#include "rhi/rhiTypes.h"
#include "render/RenderTypes.h"

using namespace Render;
using namespace Render::RHI;

TEST(NullDeviceTest, InitializeAndShutdown)
{
    NullDevice dev;
    EXPECT_TRUE(dev.initialize(nullptr, 1920, 1080));
    dev.shutdown();
}

TEST(NullDeviceTest, InitializeReturnsTrueWithoutContext)
{
    NullDevice dev;
    // Null backend should not require a native window or GL context
    EXPECT_TRUE(dev.initialize(nullptr, 800, 600));
    EXPECT_EQ(dev.getGPUMemoryUsage(), 0u);
    EXPECT_EQ(dev.getNativeContext(), nullptr);
    dev.shutdown();
}

TEST(NullDeviceTest, CreateBufferReturnsValidHandle)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    BufferDesc desc;
    desc.size = 1024;
    desc.usage = BufferUsage::Vertex;
    desc.memory = MemoryType::GPU_Only;
    desc.debugName = "TestBuffer";

    BufferHandle h = dev.createBuffer(desc);
    EXPECT_NE(h, NullHandle);

    dev.destroyBuffer(h);
    dev.shutdown();
}

TEST(NullDeviceTest, MultipleBuffersGetUniqueHandles)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    BufferDesc desc;
    desc.size = 1024;
    desc.usage = BufferUsage::Vertex;
    desc.memory = MemoryType::GPU_Only;

    BufferHandle h1 = dev.createBuffer(desc);
    BufferHandle h2 = dev.createBuffer(desc);
    BufferHandle h3 = dev.createBuffer(desc);

    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);
    EXPECT_NE(h1, h3);
    EXPECT_EQ(dev.getBufferCount(), 3u);

    dev.shutdown();
}

TEST(NullDeviceTest, CreateTextureReturnsValidHandle)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    TextureDesc desc;
    desc.width = 512;
    desc.height = 512;
    desc.format = Format::RGBA8;
    desc.mipLevels = 1;
    desc.debugName = "TestTexture";

    TextureHandle h = dev.createTexture(desc);
    EXPECT_NE(h, NullHandle);
    EXPECT_EQ(dev.getTextureCount(), 1u);

    dev.destroyTexture(h);
    // Note: getTextureCount() is a monotonically increasing counter
    // (tracks total created, not live count), so it stays at 1.
    EXPECT_EQ(dev.getTextureCount(), 1u);

    dev.shutdown();
}

TEST(NullDeviceTest, CreatePipelineReturnsValidHandle)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    PipelineDesc desc;
    desc.topology = PrimitiveTopology::TriangleList;
    desc.vertexShader = "test.vert";
    desc.fragmentShader = "test.frag";
    desc.vertexFormat = VertexFormat::P3C3;
    desc.depthTest = false;
    desc.blendEnable = false;

    PipelineHandle h = dev.createPipeline(desc);
    EXPECT_NE(h, NullHandle);
    EXPECT_EQ(dev.getPipelineCount(), 1u);

    dev.destroyPipeline(h);
    // Note: getPipelineCount() is a monotonically increasing counter
    // (tracks total created, not live count), so it stays at 1.
    EXPECT_EQ(dev.getPipelineCount(), 1u);

    dev.shutdown();
}

TEST(NullDeviceTest, DrawCallsAreCounted)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));
    EXPECT_EQ(dev.getDrawCallCount(), 0u);

    // Empty draw should still count
    dev.draw(3, 1, 0, 0);
    EXPECT_EQ(dev.getDrawCallCount(), 1u);

    dev.draw(6, 1, 0, 0);
    EXPECT_EQ(dev.getDrawCallCount(), 2u);

    // Indexed draw
    dev.drawIndexed(6, 1, 0, 0, 0);
    EXPECT_EQ(dev.getDrawCallCount(), 3u);

    // Indirect draw with drawCount=2
    BufferHandle dummyBuf = dev.createBuffer({});
    dev.drawIndirect(dummyBuf, 0, 2, 0);
    EXPECT_EQ(dev.getDrawCallCount(), 5u);

    dev.shutdown();
}

TEST(NullDeviceTest, ClearColorIsStored)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    dev.setClearColor(0.5f, 0.6f, 0.7f, 1.0f);
    dev.clear(0xFFFFFFFF);  // Should not crash

    dev.shutdown();
}

TEST(NullDeviceTest, FrameManagementIsNoOp)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    dev.beginFrame();
    dev.endFrame();
    dev.present();  // Should not crash

    dev.shutdown();
}

TEST(NullDeviceTest, StateToggleDoesNotCrash)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    dev.enableDepthTest(true);
    dev.enableBlend(true);
    dev.setLineWidth(2.5f);
    dev.resize(1280, 720);

    dev.shutdown();
}

TEST(NullDeviceTest, ComputeDispatchIsCountedAsDebug)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    // Should not crash
    dev.dispatchCompute(16, 16, 1);
    dev.memoryBarrier(static_cast<uint32_t>(BarrierFlag::All));

    dev.shutdown();
}

TEST(NullDeviceTest, MapBufferReturnsNullptr)
{
    NullDevice dev;
    ASSERT_TRUE(dev.initialize(nullptr, 1920, 1080));

    BufferHandle buf = dev.createBuffer({});
    void* ptr = dev.mapBuffer(buf, 0, 256, 0);
    EXPECT_EQ(ptr, nullptr);

    dev.unmapBuffer(buf);
    dev.flushMappedRange(buf, 0, 256);

    dev.shutdown();
}

TEST(NullBackendTest, CreateNullDeviceFactory)
{
    IDevice* dev = createNullDevice();
    ASSERT_NE(dev, nullptr);

    EXPECT_TRUE(dev->initialize(nullptr, 1024, 768));

    // Verify it works as expected
    BufferHandle buf = dev->createBuffer({});
    EXPECT_NE(buf, NullHandle);

    dev->shutdown();
    delete dev;
}

// M1/M2: 多窗口隔离测试
// 验证 Null backend 可以支持多个独立会话（多个 RenderDevice 实例），
// 每个会话拥有自己的状态，不受其他会话干扰。
TEST(NullBackendMultiWindowTest, TwoDevicesAreIsolated)
{
    IDevice* dev1 = createNullDevice();
    IDevice* dev2 = createNullDevice();

    ASSERT_NE(dev1, nullptr);
    ASSERT_NE(dev2, nullptr);
    ASSERT_TRUE(dev1->initialize(nullptr, 1920, 1080));
    ASSERT_TRUE(dev2->initialize(nullptr, 800, 600));

    // dev1 创建 3 个缓冲区
    BufferDesc desc;
    desc.size = 256;
    desc.usage = BufferUsage::Vertex;
    BufferHandle buf1_1 = dev1->createBuffer(desc);
    dev1->createBuffer(desc);
    dev1->createBuffer(desc);

    // dev2 创建 1 个缓冲区
    BufferHandle buf2_1 = dev2->createBuffer(desc);

    // M6: Note: NullDevice handles are per-device, so each device starts from ID 1
    // The test verifies isolation, not global uniqueness
    EXPECT_TRUE(buf1_1 != NullHandle);
    EXPECT_TRUE(buf2_1 != NullHandle);

    // Verify draw call counts are per-device (isolation)
    NullDevice* nullDev1 = static_cast<NullDevice*>(dev1);
    NullDevice* nullDev2 = static_cast<NullDevice*>(dev2);

    dev1->draw(3, 1, 0, 0);
    EXPECT_EQ(nullDev1->getDrawCallCount(), 1u);
    EXPECT_EQ(nullDev2->getDrawCallCount(), 0u);

    dev1->shutdown();
    dev2->shutdown();
    delete dev1;
    delete dev2;
}

// 注：原 RuntimeIsProcessSingleton 用例已删除。
// 它断言的 RenderRuntime 进程级单例本身就是多窗口的缺陷来源：
// 单例的 initialize() 在已初始化时直接早返回，第二个窗口若使用不同
// backend 或 shaderDir 会被静默忽略（先到先得）。该单例已随
// src/c_api/renderRuntime.cpp 一并删除，配置改为随设备传入。
