/**
 * @file RhiCoreTests.cpp
 * @brief 新 RHI 契约测试（跑在 Null 后端上，不需要 GPU/窗口）
 *
 * 这些用例断言的是**与后端无关的语义**：句柄生命周期、帧配对、
 * RenderPass 嵌套、越界拒绝、能力查询、不做静默回退。
 * Metal/Vulkan 后端落地后必须原样通过同一批用例（换 BackendKind 即可），
 * 这是「三个后端语义一致」的判定基准。
 */

#include <gtest/gtest.h>

#include "rhi/rhiBackendFactory.h"
#include "rhi/rhiCommandList.h"
#include "rhi/rhiGpuDevice.h"
#include "rhi/rhiSurface.h"

#include <cstring>
#include <string>
#include <vector>

using namespace Render::RHI;

namespace
{
    /// 收集日志以便断言「失败路径确实报告了原因」，而不是静默返回
    struct LogSink
    {
        std::vector<std::string> errors;
        std::vector<std::string> warns;

        static void callback(LogLevel level, const char* message, void* userData)
        {
            auto* sink = static_cast<LogSink*>(userData);
            if (level == LogLevel::Error)
            {
                sink->errors.emplace_back(message ? message : "");
            }
            else if (level == LogLevel::Warn)
            {
                sink->warns.emplace_back(message ? message : "");
            }
        }
    };

    struct NullDeviceFixture : public ::testing::Test
    {
        LogSink sink;
        IGpuDevice* device = nullptr;

        void SetUp() override
        {
            DeviceDesc desc{};
            desc.backend = BackendKind::Null;
            desc.logCallback = &LogSink::callback;
            desc.logUserData = &sink;
            device = createDevice(desc);
            ASSERT_NE(device, nullptr);
        }

        void TearDown() override
        {
            destroyDevice(device);
            device = nullptr;
        }

        ISurface* makeSurface(uint32_t width = 800, uint32_t height = 600)
        {
            SurfaceDesc desc{};
            desc.window.kind = NativeWindow::Kind::ForeignGlContext;
            desc.initialExtent = { width, height };
            desc.depthFormat = Format::D32Float;
            return device->createSurface(desc);
        }
    };
}  // namespace

// ==================== 工厂：不做静默回退 ====================

TEST(RhiFactory, NullBackendAlwaysAvailable)
{
    EXPECT_TRUE(isBackendAvailable(BackendKind::Null));
}

TEST(RhiFactory, UnimplementedBackendsReturnNullptrInsteadOfFallingBack)
{
    // 旧实现在 Vulkan/Metal 不可用时静默换 Null，调用方拿到「创建成功」
    // 却画面全黑。这里断言现在会明确失败并给出原因。
    for (BackendKind backend : { BackendKind::Metal, BackendKind::Vulkan })
    {
        LogSink sink;
        DeviceDesc desc{};
        desc.backend = backend;
        desc.logCallback = &LogSink::callback;
        desc.logUserData = &sink;

        EXPECT_EQ(createDevice(desc), nullptr) << backendName(backend);
        EXPECT_FALSE(sink.errors.empty()) << "失败必须报告原因：" << backendName(backend);
        EXPECT_FALSE(isBackendAvailable(backend));
    }
}

TEST(RhiFactory, PreferredBackendIsActuallyImplemented)
{
    EXPECT_TRUE(isBackendAvailable(preferredBackend()));
}

TEST(RhiFactory, ResultAndBackendNamesAreStable)
{
    EXPECT_STREQ(backendName(BackendKind::OpenGL), "OpenGL");
    EXPECT_STREQ(resultName(RhiResult::Ok), "Ok");
    EXPECT_STREQ(resultName(RhiResult::ErrorSwapchainOutOfDate), "ErrorSwapchainOutOfDate");
}

// ==================== 能力查询 ====================

TEST_F(NullDeviceFixture, CapabilitiesReportNullBackend)
{
    const Capabilities& caps = device->capabilities();
    EXPECT_EQ(caps.backend, BackendKind::Null);
    EXPECT_EQ(caps.maxPushConstantBytes, kMaxPushConstantBytes);
    EXPECT_GT(caps.maxTextureSize, 0u);
    EXPECT_STRNE(caps.deviceName, "");
}

// ==================== 句柄生命周期 ====================

TEST_F(NullDeviceFixture, HandlesAreNeverZeroAndDestroyedHandlesStopResolving)
{
    BufferDesc desc{};
    desc.size = 256;
    desc.usage = BufferUsage::Vertex;
    desc.access = MemoryAccess::CpuToGpu;

    const BufferHandle first = device->createBuffer(desc);
    ASSERT_TRUE(first.valid());
    EXPECT_NE(first.value, 0u);

    device->destroyBuffer(first);

    // 已销毁的句柄不能再被接受：世代号已自增
    const uint8_t payload[4] = { 1, 2, 3, 4 };
    EXPECT_EQ(device->writeBuffer(first, 0, payload, sizeof(payload)), RhiResult::ErrorInvalidArgument);

    // 槽位复用后，旧句柄也不得命中新资源（这正是世代机制要防的 use-after-free）
    const BufferHandle second = device->createBuffer(desc);
    ASSERT_TRUE(second.valid());
    EXPECT_NE(second.value, first.value);
    EXPECT_EQ(device->writeBuffer(first, 0, payload, sizeof(payload)), RhiResult::ErrorInvalidArgument);
    EXPECT_EQ(device->writeBuffer(second, 0, payload, sizeof(payload)), RhiResult::Ok);

    device->destroyBuffer(second);
}

TEST_F(NullDeviceFixture, DoubleDestroyIsReportedNotSilent)
{
    BufferDesc desc{};
    desc.size = 64;
    const BufferHandle buffer = device->createBuffer(desc);
    device->destroyBuffer(buffer);

    const size_t warnsBefore = sink.warns.size();
    device->destroyBuffer(buffer);
    EXPECT_GT(sink.warns.size(), warnsBefore) << "重复销毁应留下告警";
}

// ==================== 缓冲区数据 ====================

TEST_F(NullDeviceFixture, BufferWriteAndMapRoundTrip)
{
    BufferDesc desc{};
    desc.size = 128;
    desc.usage = BufferUsage::Vertex;
    desc.access = MemoryAccess::CpuToGpu;
    const BufferHandle buffer = device->createBuffer(desc);
    ASSERT_TRUE(buffer.valid());

    const uint32_t values[4] = { 0xAABBCCDDu, 2u, 3u, 4u };
    ASSERT_EQ(device->writeBuffer(buffer, 16, values, sizeof(values)), RhiResult::Ok);

    const MappedRange range = device->mapBuffer(buffer, 16, sizeof(values));
    ASSERT_NE(range.ptr, nullptr);
    EXPECT_EQ(range.offset, 16u);
    EXPECT_EQ(range.size, sizeof(values));
    EXPECT_EQ(std::memcmp(range.ptr, values, sizeof(values)), 0);
    device->unmapBuffer(buffer);

    device->destroyBuffer(buffer);
}

TEST_F(NullDeviceFixture, BufferWriteRejectsOutOfRange)
{
    BufferDesc desc{};
    desc.size = 32;
    const BufferHandle buffer = device->createBuffer(desc);

    const uint8_t payload[16] = {};
    EXPECT_EQ(device->writeBuffer(buffer, 24, payload, sizeof(payload)), RhiResult::ErrorInvalidArgument);
    EXPECT_EQ(device->writeBuffer(buffer, 16, payload, sizeof(payload)), RhiResult::Ok);

    device->destroyBuffer(buffer);
}

TEST_F(NullDeviceFixture, GpuOnlyBufferCannotBeMapped)
{
    BufferDesc desc{};
    desc.size = 32;
    desc.access = MemoryAccess::GpuOnly;
    const BufferHandle buffer = device->createBuffer(desc);

    EXPECT_EQ(device->mapBuffer(buffer, 0, 0).ptr, nullptr);
    EXPECT_FALSE(sink.errors.empty());

    device->destroyBuffer(buffer);
}

// ==================== 纹理 ====================

TEST_F(NullDeviceFixture, TextureWriteReadRoundTripUsesTopLeftOrigin)
{
    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::R8Unorm;
    desc.usage = TextureUsage::Sampled;
    const TextureHandle texture = device->createTexture(desc);
    ASSERT_TRUE(texture.valid());

    // 只写第 1 行（y=1）的中间两列，用于验证行距与偏移都没算错
    const uint8_t row[2] = { 0x11, 0x22 };
    Rect2D region{ 1, 1, 2, 1 };
    ASSERT_EQ(device->writeTexture(texture, 0, region, row, sizeof(row)), RhiResult::Ok);

    uint8_t readback[2] = {};
    uint32_t rowPitch = 0;
    ASSERT_EQ(device->readTexture(texture, region, readback, sizeof(readback), &rowPitch), RhiResult::Ok);
    EXPECT_EQ(rowPitch, 2u);
    EXPECT_EQ(readback[0], 0x11);
    EXPECT_EQ(readback[1], 0x22);

    // 相邻行仍应为 0：证明写入没有溢出到别的行
    uint8_t neighbour[2] = { 0xFF, 0xFF };
    Rect2D above{ 1, 0, 2, 1 };
    ASSERT_EQ(device->readTexture(texture, above, neighbour, sizeof(neighbour), nullptr), RhiResult::Ok);
    EXPECT_EQ(neighbour[0], 0);
    EXPECT_EQ(neighbour[1], 0);

    device->destroyTexture(texture);
}

TEST_F(NullDeviceFixture, TextureRejectsOutOfBoundsRegion)
{
    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::RGBA8Unorm;
    const TextureHandle texture = device->createTexture(desc);

    const uint8_t payload[64] = {};
    Rect2D region{ 2, 2, 4, 4 };  // 越过右下边界
    EXPECT_EQ(device->writeTexture(texture, 0, region, payload, sizeof(payload)),
              RhiResult::ErrorInvalidArgument);

    device->destroyTexture(texture);
}

// ==================== 表面 ====================

TEST_F(NullDeviceFixture, SurfaceAcquirePresentMustBePaired)
{
    ISurface* surface = makeSurface();
    ASSERT_NE(surface, nullptr);

    // 未 acquire 就 present 属于违约
    EXPECT_EQ(surface->present(), RhiResult::ErrorInvalidArgument);

    ASSERT_EQ(surface->acquireNextImage(), RhiResult::Ok);
    EXPECT_EQ(surface->present(), RhiResult::Ok);

    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, MinimizedSurfaceReportsOutOfDateInsteadOfRenderingIntoNothing)
{
    ISurface* surface = makeSurface();
    ASSERT_EQ(surface->resize({ 0, 0 }), RhiResult::Ok);
    EXPECT_EQ(surface->acquireNextImage(), RhiResult::ErrorSwapchainOutOfDate);

    ASSERT_EQ(surface->resize({ 640, 480 }), RhiResult::Ok);
    EXPECT_EQ(surface->extent().width, 640u);
    EXPECT_EQ(surface->acquireNextImage(), RhiResult::Ok);

    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, MultipleSurfacesShareOneDevice)
{
    // 多窗口的核心诉求：N 个表面共用一个设备与其全部资源
    ISurface* a = makeSurface(800, 600);
    ISurface* b = makeSurface(1024, 768);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(a->extent().width, 800u);
    EXPECT_EQ(b->extent().width, 1024u);

    // 资源在设备上，两个表面都能用同一批句柄渲染
    BufferDesc bd{};
    bd.size = 64;
    const BufferHandle shared = device->createBuffer(bd);
    ASSERT_TRUE(shared.valid());

    for (ISurface* surface : { a, b })
    {
        ASSERT_EQ(surface->acquireNextImage(), RhiResult::Ok);
        ICommandList* cmd = device->beginFrame(surface);
        ASSERT_NE(cmd, nullptr);
        ASSERT_EQ(device->submitFrame(), RhiResult::Ok);
        ASSERT_EQ(surface->present(), RhiResult::Ok);
    }

    device->destroyBuffer(shared);
    device->destroySurface(a);
    device->destroySurface(b);
}

TEST_F(NullDeviceFixture, DestroyingForeignSurfaceIsReported)
{
    ISurface* surface = makeSurface();
    device->destroySurface(surface);

    const size_t errorsBefore = sink.errors.size();
    device->destroySurface(surface);  // 已经不属于本设备
    EXPECT_GT(sink.errors.size(), errorsBefore);
}

// ==================== 帧与 RenderPass ====================

TEST_F(NullDeviceFixture, FrameBeginSubmitMustBePaired)
{
    ISurface* surface = makeSurface();
    ASSERT_EQ(device->submitFrame(), RhiResult::ErrorInvalidArgument);

    ASSERT_NE(device->beginFrame(surface), nullptr);
    EXPECT_EQ(device->beginFrame(surface), nullptr) << "重入 beginFrame 必须失败";
    EXPECT_EQ(device->submitFrame(), RhiResult::Ok);

    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, UnclosedRenderPassBlocksSubmit)
{
    ISurface* surface = makeSurface();
    ICommandList* cmd = device->beginFrame(surface);
    ASSERT_NE(cmd, nullptr);

    RenderPassBeginDesc pass{};
    pass.colorAttachmentCount = 1;
    pass.extent = surface->extent();
    ASSERT_EQ(cmd->beginRenderPass(pass), RhiResult::Ok);

    // 嵌套开启同样违约
    EXPECT_EQ(cmd->beginRenderPass(pass), RhiResult::ErrorInvalidArgument);
    EXPECT_EQ(device->submitFrame(), RhiResult::ErrorInvalidArgument);

    cmd->endRenderPass();
    EXPECT_EQ(device->submitFrame(), RhiResult::Ok);

    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, DrawOutsideRenderPassIsRejectedAndNotCounted)
{
    ISurface* surface = makeSurface();
    ICommandList* cmd = device->beginFrame(surface);

    cmd->draw(3, 1, 0, 0);
    EXPECT_EQ(cmd->stats().drawCalls, 0u);
    EXPECT_FALSE(sink.errors.empty());

    RenderPassBeginDesc pass{};
    pass.extent = surface->extent();
    ASSERT_EQ(cmd->beginRenderPass(pass), RhiResult::Ok);
    cmd->draw(3, 1, 0, 0);
    cmd->draw(0, 1, 0, 0);  // 顶点数 0：不算一次绘制
    cmd->endRenderPass();
    EXPECT_EQ(cmd->stats().drawCalls, 1u);

    ASSERT_EQ(device->submitFrame(), RhiResult::Ok);
    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, StatsResetEachFrame)
{
    ISurface* surface = makeSurface();

    for (int frame = 0; frame < 2; ++frame)
    {
        ICommandList* cmd = device->beginFrame(surface);
        ASSERT_NE(cmd, nullptr);
        RenderPassBeginDesc pass{};
        pass.extent = surface->extent();
        ASSERT_EQ(cmd->beginRenderPass(pass), RhiResult::Ok);
        cmd->draw(3, 1, 0, 0);
        cmd->endRenderPass();
        EXPECT_EQ(cmd->stats().drawCalls, 1u) << "第 " << frame << " 帧统计未重置";
        ASSERT_EQ(device->submitFrame(), RhiResult::Ok);
    }

    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, PushConstantsRejectOverflow)
{
    ISurface* surface = makeSurface();
    ICommandList* cmd = device->beginFrame(surface);

    std::vector<uint8_t> payload(kMaxPushConstantBytes, 0x5A);
    const size_t errorsBefore = sink.errors.size();
    cmd->pushConstants(0, kMaxPushConstantBytes, payload.data());
    EXPECT_EQ(sink.errors.size(), errorsBefore) << "刚好写满上限不应报错";

    cmd->pushConstants(4, kMaxPushConstantBytes, payload.data());
    EXPECT_GT(sink.errors.size(), errorsBefore) << "越界写入必须报错";

    ASSERT_EQ(device->submitFrame(), RhiResult::Ok);
    device->destroySurface(surface);
}

TEST_F(NullDeviceFixture, ComputeDispatchMustBeOutsideRenderPass)
{
    ISurface* surface = makeSurface();
    ICommandList* cmd = device->beginFrame(surface);

    cmd->dispatchCompute(1, 1, 1);
    EXPECT_EQ(cmd->stats().computeDispatches, 1u);

    RenderPassBeginDesc pass{};
    pass.extent = surface->extent();
    ASSERT_EQ(cmd->beginRenderPass(pass), RhiResult::Ok);
    cmd->dispatchCompute(1, 1, 1);
    cmd->endRenderPass();
    EXPECT_EQ(cmd->stats().computeDispatches, 1u) << "RenderPass 内的 dispatch 应被拒绝";

    ASSERT_EQ(device->submitFrame(), RhiResult::Ok);
    device->destroySurface(surface);
}

// ==================== 绑定组校验 ====================

TEST_F(NullDeviceFixture, BindGroupRejectsInvalidHandles)
{
    BufferHandle dangling{};
    dangling.value = 0xDEADBEEF;

    BufferBinding binding{};
    binding.binding = 0;
    binding.buffer = dangling;

    BindGroupDesc desc{};
    desc.buffers = &binding;
    desc.bufferCount = 1;

    EXPECT_FALSE(device->createBindGroup(desc).valid());
    EXPECT_FALSE(sink.errors.empty());
}

TEST_F(NullDeviceFixture, ShaderRequiresMatchingLanguage)
{
    const char* source = "#version 330 core\nvoid main(){}\n";

    ShaderDesc bad{};
    bad.language = ShaderLanguage::SpirV;
    bad.data = source;
    bad.sizeBytes = std::strlen(source);
    EXPECT_FALSE(device->createShader(bad).valid());

    ShaderDesc good{};
    good.language = device->capabilities().acceptedShaderLanguage;
    good.data = source;
    good.sizeBytes = std::strlen(source);
    const ShaderHandle handle = device->createShader(good);
    EXPECT_TRUE(handle.valid());
    device->destroyShader(handle);
}

TEST_F(NullDeviceFixture, GraphicsPipelineRequiresValidShaders)
{
    GraphicsPipelineDesc desc{};
    EXPECT_FALSE(device->createGraphicsPipeline(desc).valid());
    EXPECT_FALSE(sink.errors.empty());
}
