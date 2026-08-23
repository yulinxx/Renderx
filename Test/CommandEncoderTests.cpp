/**
 * @file CommandEncoderTests.cpp
 * @brief CommandEncoder integration tests using Null backend
 *
 * Tests the CommandEncoder's command collection and reset
 * using the NullDevice (no GPU required).
 */
#include <gtest/gtest.h>
#include "core/commandEncoder.h"
#include "rhi/rhiNull.h"
#include "rhi/rhiDevice.h"
#include "rhi/rhiTypes.h"

using namespace Render;
using namespace Render::core;
using namespace Render::RHI;

class CommandEncoderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        device = std::make_unique<NullDevice>();
        ASSERT_TRUE(device->initialize(nullptr, 800, 600));
        encoder = std::make_unique<CommandEncoder>();
        ASSERT_TRUE(encoder->initialize(device.get()));
    }

    void TearDown() override
    {
        if (encoder)
        {
            encoder->shutdown();
        }
        if (device)
        {
            device->shutdown();
        }
    }

    std::unique_ptr<NullDevice> device;
    std::unique_ptr<CommandEncoder> encoder;
};

TEST_F(CommandEncoderTest, InitializeAndShutdown)
{
    EXPECT_NE(encoder.get(), nullptr);
    encoder->shutdown();
    // Double shutdown should be safe
    encoder->shutdown();
}

TEST_F(CommandEncoderTest, ResetClearsCommands)
{
    encoder->reset();
    encoder->submitOverlay(PrimitiveType::TriangleList, 0, 4, 100);
    encoder->reset();
    EXPECT_EQ(encoder->getCommandCount(), 0u);
}

TEST_F(CommandEncoderTest, SubmitOverlay)
{
    encoder->reset();
    encoder->submitOverlay(PrimitiveType::TriangleList, 0, 4, 100);
    EXPECT_EQ(encoder->getCommandCount(), 1u);
}

TEST_F(CommandEncoderTest, SubmitWorld)
{
    encoder->reset();
    encoder->submitWorld(PrimitiveType::TriangleList, 0, 0, 1, 0, 1.0f);
    EXPECT_EQ(encoder->getCommandCount(), 1u);
}

TEST_F(CommandEncoderTest, SubmitMultipleCommands)
{
    encoder->reset();
    for (int i = 0; i < 10; ++i)
    {
        encoder->submitOverlay(PrimitiveType::TriangleList, i * 4, 4, 100);
    }
    EXPECT_EQ(encoder->getCommandCount(), 10u);
}

TEST_F(CommandEncoderTest, ResetAfterSubmit)
{
    encoder->submitOverlay(PrimitiveType::TriangleList, 0, 4, 100);
    encoder->submitWorld(PrimitiveType::TriangleList, 0, 0, 1, 0, 1.0f);
    EXPECT_EQ(encoder->getCommandCount(), 2u);
    encoder->reset();
    EXPECT_EQ(encoder->getCommandCount(), 0u);
}

TEST_F(CommandEncoderTest, SetPipelineStateManager)
{
    // Should accept null without crash
    encoder->setPipelineStateManager(nullptr);
}

TEST_F(CommandEncoderTest, SetDrawBatcher)
{
    // Should accept null without crash
    encoder->setDrawBatcher(nullptr);
}

TEST_F(CommandEncoderTest, GetBatchCount)
{
    encoder->reset();
    EXPECT_EQ(encoder->getBatchCount(), 0u);
    encoder->submitOverlay(PrimitiveType::TriangleList, 0, 4, 100);
    encoder->submitOverlay(PrimitiveType::TriangleList, 4, 4, 100);
}

TEST_F(CommandEncoderTest, MultipleFrames)
{
    for (int frame = 0; frame < 5; ++frame)
    {
        encoder->reset();
        encoder->submitOverlay(PrimitiveType::TriangleList, frame * 4, 4, 100);
        encoder->submitWorld(PrimitiveType::TriangleList, 0, 0, 1, 0, 1.0f);
        EXPECT_EQ(encoder->getCommandCount(), 2u);
    }
}
