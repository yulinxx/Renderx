/**
 * @file RenderGraphTests.cpp
 * @brief RenderGraph integration tests using Null backend
 *
 * Tests the RenderGraph's pass management, execution, and enable/disable
 * functionality using the NullDevice (no GPU required).
 */
#include <gtest/gtest.h>
#include "core/renderGraph.h"
#include "rhi/rhiNull.h"
#include "rhi/rhiDevice.h"

using namespace Render;
using namespace Render::core;
using namespace Render::RHI;

class RenderGraphTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        device = std::make_unique<NullDevice>();
        ASSERT_TRUE(device->initialize(nullptr, 800, 600));
        graph = std::make_unique<RenderGraph>();
        ASSERT_TRUE(graph->initialize(device.get()));
    }

    void TearDown() override
    {
        if (graph)
        {
            graph->shutdown();
        }
        if (device)
        {
            device->shutdown();
        }
    }

    std::unique_ptr<NullDevice> device;
    std::unique_ptr<RenderGraph> graph;
};

TEST_F(RenderGraphTest, InitializeAndShutdown)
{
    EXPECT_NE(graph.get(), nullptr);
    graph->shutdown();
    // Double shutdown should be safe
    graph->shutdown();
}

TEST_F(RenderGraphTest, AddSinglePass)
{
    PassDesc desc;
    desc.name = "TestPass";
    desc.enabled = true;
    desc.onExecute = [](IDevice*) {};
    graph->addPass(desc);
    EXPECT_EQ(graph->getPassCount(), 1u);
}

TEST_F(RenderGraphTest, AddMultiplePasses)
{
    const char* names[] = {"Pass0", "Pass1", "Pass2", "Pass3", "Pass4"};
    for (int i = 0; i < 5; ++i)
    {
        PassDesc desc;
        desc.name = names[i];
        desc.enabled = true;
        desc.onExecute = [](IDevice*) {};
        graph->addPass(desc);
    }
    EXPECT_EQ(graph->getPassCount(), 5u);
}

TEST_F(RenderGraphTest, ClearRemovesAllPasses)
{
    PassDesc desc;
    desc.name = "TestPass";
    desc.onExecute = [](IDevice*) {};
    graph->addPass(desc);
    EXPECT_EQ(graph->getPassCount(), 1u);
    graph->clear();
    EXPECT_EQ(graph->getPassCount(), 0u);
}

TEST_F(RenderGraphTest, ExecuteEmptyGraph)
{
    // Executing an empty graph should be safe
    graph->execute(device.get());
    EXPECT_EQ(graph->getExecutedPassCount(), 0u);
}

TEST_F(RenderGraphTest, ExecuteSinglePass)
{
    bool executed = false;
    PassDesc desc;
    desc.name = "ExecPass";
    desc.onExecute = [&executed](IDevice*) { executed = true; };
    graph->addPass(desc);

    graph->execute(device.get());
    EXPECT_TRUE(executed);
    EXPECT_EQ(graph->getExecutedPassCount(), 1u);
}

TEST_F(RenderGraphTest, ExecutePassesInOrder)
{
    std::vector<int> order;
    const char* names[] = {"Pass0", "Pass1", "Pass2"};
    for (int i = 0; i < 3; ++i)
    {
        PassDesc desc;
        desc.name = names[i];
        int idx = i;
        desc.onExecute = [&order, idx](IDevice*) { order.push_back(idx); };
        graph->addPass(desc);
    }

    graph->execute(device.get());
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0);
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 2);
}

TEST_F(RenderGraphTest, DisablePass)
{
    bool executed1 = false;
    bool executed2 = false;

    PassDesc desc1;
    desc1.name = "Pass1";
    desc1.onExecute = [&executed1](IDevice*) { executed1 = true; };
    graph->addPass(desc1);

    PassDesc desc2;
    desc2.name = "Pass2";
    desc2.onExecute = [&executed2](IDevice*) { executed2 = true; };
    graph->addPass(desc2);

    // Disable pass 1
    graph->setPassEnabled(0, false);
    EXPECT_FALSE(graph->isPassEnabled(0));
    EXPECT_TRUE(graph->isPassEnabled(1));

    graph->execute(device.get());
    EXPECT_FALSE(executed1);
    EXPECT_TRUE(executed2);
    EXPECT_EQ(graph->getExecutedPassCount(), 1u);
}

TEST_F(RenderGraphTest, GetPassName)
{
    PassDesc desc;
    desc.name = "NamedPass";
    desc.onExecute = [](IDevice*) {};
    graph->addPass(desc);
    EXPECT_STREQ(graph->getPassName(0), "NamedPass");
}

TEST_F(RenderGraphTest, OnSetupCalledBeforeOnExecute)
{
    bool setupCalled = false;
    bool executeCalled = false;
    bool setupBeforeExecute = false;

    PassDesc desc;
    desc.name = "SetupOrder";
    desc.onSetup = [&](IDevice*) {
        setupCalled = true;
        setupBeforeExecute = !executeCalled;
    };
    desc.onExecute = [&](IDevice*) {
        executeCalled = true;
    };
    graph->addPass(desc);

    graph->execute(device.get());
    EXPECT_TRUE(setupCalled);
    EXPECT_TRUE(executeCalled);
    EXPECT_TRUE(setupBeforeExecute);
}

TEST_F(RenderGraphTest, ExecuteMultipleFrames)
{
    int frameCount = 0;
    PassDesc desc;
    desc.name = "Counter";
    desc.onExecute = [&frameCount](IDevice*) { frameCount++; };
    graph->addPass(desc);

    for (int i = 0; i < 10; ++i)
    {
        graph->execute(device.get());
    }
    EXPECT_EQ(frameCount, 10);
}

TEST_F(RenderGraphTest, ResourceConflictCheck)
{
    // Should not crash even with no passes
    graph->checkResourceConflicts();

    // Add passes with no resource declarations
    PassDesc desc;
    desc.name = "NoResources";
    desc.onExecute = [](IDevice*) {};
    graph->addPass(desc);
    graph->checkResourceConflicts();
}
