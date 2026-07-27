/**
 * @file draw_batcher.cpp
 * @brief 绘制合批器实现
 */
#include "draw_batcher.h"
#include "Log/SyLogger.h"

namespace render {
namespace core {

void DrawBatcher::initialize(rhi::IDevice* device)
{
    if (m_initialized || !device)
        return;

    m_device = device;
    m_initialized = true;

    SY_INFO("[DrawBatcher] initialized");
}

void DrawBatcher::shutdown()
{
    if (!m_initialized)
        return;

    if (m_indirectBuffer != rhi::NullHandle && m_device)
    {
        m_device->destroyBuffer(m_indirectBuffer);
        m_indirectBuffer = rhi::NullHandle;
    }

    m_indirectCmds.clear();
    m_indirectCmds.shrink_to_fit();
    m_cmdPipelines.clear();
    m_cmdPipelines.shrink_to_fit();
    m_groups.clear();
    m_groups.shrink_to_fit();

    m_bufferCapacity = 0;
    m_cmdCount = 0;
    m_device = nullptr;
    m_initialized = false;

    SY_INFO("[DrawBatcher] shutdown");
}

void DrawBatcher::reset()
{
    m_indirectCmds.clear();
    m_cmdPipelines.clear();
    m_groups.clear();
    m_cmdCount = 0;
}

void DrawBatcher::appendOverlayCmd(rhi::PipelineHandle pipeline,
                                    uint32_t vertexOffset, uint32_t vertexCount)
{
    if (!m_initialized)
        return;

    DrawIndirectCmd cmd;
    cmd.vertexCount = vertexCount;
    cmd.instanceCount = 1;
    cmd.firstVertex = vertexOffset;
    cmd.baseInstance = 0;

    m_indirectCmds.push_back(cmd);
    m_cmdPipelines.push_back(pipeline);
    ++m_cmdCount;
}

const std::vector<BatchGroup>& DrawBatcher::build()
{
    m_groups.clear();

    if (m_indirectCmds.empty() || !m_device)
        return m_groups;

    const uint32_t cmdCount = static_cast<uint32_t>(m_indirectCmds.size());

    // 确保 indirect GPU buffer 足够
    if (cmdCount > m_bufferCapacity)
    {
        if (m_indirectBuffer != rhi::NullHandle)
        {
            m_device->destroyBuffer(m_indirectBuffer);
            m_indirectBuffer = rhi::NullHandle;
        }

        uint32_t newCap = m_bufferCapacity;
        if (newCap == 0) newCap = 64;
        while (newCap < cmdCount) newCap *= 2;

        rhi::BufferDesc desc;
        desc.size = newCap * sizeof(DrawIndirectCmd);
        desc.usage = rhi::BufferUsage::Indirect;
        desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
        desc.debugName = "DrawBatcher_Indirect";
        m_indirectBuffer = m_device->createBuffer(desc);
        m_bufferCapacity = newCap;
    }

    // 上传所有 indirect 命令
    m_device->uploadBuffer(m_indirectBuffer, 0,
        cmdCount * sizeof(DrawIndirectCmd),
        m_indirectCmds.data());

    // 按 pipeline 分组：连续相同 pipeline 的命令合并为一组
    uint32_t groupStart = 0;
    rhi::PipelineHandle currentPipeline = m_cmdPipelines[0];

    for (uint32_t i = 1; i <= cmdCount; ++i)
    {
        bool endOfGroup = (i == cmdCount) || (m_cmdPipelines[i] != currentPipeline);
        if (endOfGroup)
        {
            uint32_t groupCount = i - groupStart;
            if (groupCount > 0)
            {
                BatchGroup bg;
                bg.pipeline = currentPipeline;
                bg.indirectOffset = groupStart * sizeof(DrawIndirectCmd);
                bg.drawCount = groupCount;
                bg.space = DrawSpace::Overlay;
                bg.vertexBuffer = rhi::NullHandle; // 由调用方绑定
                m_groups.push_back(bg);
            }

            if (i < cmdCount)
            {
                groupStart = i;
                currentPipeline = m_cmdPipelines[i];
            }
        }
    }

    SY_DEBUGF("[DrawBatcher] build: %u commands -> %u groups", cmdCount, m_groups.size());
    return m_groups;
}

} // namespace core
} // namespace render
