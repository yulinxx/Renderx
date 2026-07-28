/**
 * @file command_encoder.cpp
 * @brief 统一命令编码器实现
 *
 * Phase 3 核心实现，统一收集 overlay / world 绘制命令并按 batch key 排序执行。
 */
#include "command_encoder.h"
#include "pipeline_state_manager.h"
#include "draw_batcher.h"
#include "Log/SyLogger.h"
#include <algorithm>
#include <cstring>

namespace render
{
    namespace core
    {
        // ============================================================================
        // 构造 / 析构函数
        // ============================================================================

        CommandEncoder::CommandEncoder() = default;
        CommandEncoder::~CommandEncoder()
        {
            shutdown();
        }

        // ============================================================================
        // 生命周期
        // ============================================================================

        void CommandEncoder::initialize(rhi::IDevice* device)
        {
            if (m_initialized || !device)
                return;

            m_device = device;

            // ------------------------------------------------------------------------
            // 创建 overlay pipeline
            // ------------------------------------------------------------------------
            {
                rhi::PipelineDesc lineDesc;
                lineDesc.topology = rhi::PrimitiveTopology::LineList;
                lineDesc.vertexShader = "overlay_vert";
                lineDesc.fragmentShader = "overlay_frag";
                lineDesc.computeShader = nullptr;
                lineDesc.vertexFormat = rhi::VertexFormat::P3C4;
                lineDesc.depthTest = false;
                lineDesc.depthWrite = false;
                lineDesc.blendEnable = true;
                lineDesc.srcBlend = rhi::BlendFactor::SrcAlpha;
                lineDesc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
                lineDesc.depthFunc = rhi::CompareFunc::Always;
                m_overlayLinePipeline = m_psm
                    ? m_psm->getOrCreatePipeline(lineDesc)
                    : device->createPipeline(lineDesc);

                rhi::PipelineDesc triDesc = lineDesc;
                triDesc.topology = rhi::PrimitiveTopology::TriangleList;
                m_overlayTriPipeline = m_psm
                    ? m_psm->getOrCreatePipeline(triDesc)
                    : device->createPipeline(triDesc);
            }

            // ------------------------------------------------------------------------
            // 创建 world pipeline（7 种 topology）
            // ------------------------------------------------------------------------
            static const char* kWorldVert = "passthrough_vert";
            static const char* kWorldFrag = "passthrough_frag";

            static const rhi::PrimitiveTopology kTopoMap[PRIMITIVE_TYPE_COUNT] = {
                rhi::PrimitiveTopology::PointList,
                rhi::PrimitiveTopology::LineList,
                rhi::PrimitiveTopology::LineStrip,
                rhi::PrimitiveTopology::LineLoop,
                rhi::PrimitiveTopology::TriangleList,
                rhi::PrimitiveTopology::TriangleStrip,
                rhi::PrimitiveTopology::TriangleFan,
            };

            for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i)
            {
                rhi::PipelineDesc desc;
                desc.topology = kTopoMap[i];
                desc.vertexShader = kWorldVert;
                desc.fragmentShader = kWorldFrag;
                desc.computeShader = nullptr;
                desc.vertexFormat = rhi::VertexFormat::P3C3;
                desc.depthTest = false;
                desc.depthWrite = false;
                desc.blendEnable = true;
                desc.srcBlend = rhi::BlendFactor::SrcAlpha;
                desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
                desc.depthFunc = rhi::CompareFunc::Always;
                m_worldPipelines[i] = m_psm
                    ? m_psm->getOrCreatePipeline(desc)
                    : device->createPipeline(desc);
            }

            m_commands.reserve(256);
            m_initialized = true;

            SY_INFOF("[CommandEncoder] Initialized with %u world pipelines + 2 overlay pipelines (PSM=%s)",
                PRIMITIVE_TYPE_COUNT, m_psm ? "yes" : "no");
        }

        void CommandEncoder::setPipelineStateManager(PipelineStateManager* psm)
        {
            m_psm = psm;
        }

        void CommandEncoder::setDrawBatcher(DrawBatcher* batcher)
        {
            m_drawBatcher = batcher;
        }

        void CommandEncoder::shutdown()
        {
            if (!m_initialized || !m_device)
                return;

            if (m_overlayLinePipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_overlayLinePipeline);
                m_overlayLinePipeline = {};
            }
            if (m_overlayTriPipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_overlayTriPipeline);
                m_overlayTriPipeline = {};
            }

            for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i)
            {
                if (m_worldPipelines[i] != rhi::NullHandle)
                {
                    m_device->destroyPipeline(m_worldPipelines[i]);
                    m_worldPipelines[i] = {};
                }
            }

            m_commands.clear();
            m_commands.shrink_to_fit();
            m_initialized = false;
            m_device = nullptr;
            m_lastBatchCount = 0;

            SY_INFOF("[CommandEncoder] Shutdown complete");
        }

        void CommandEncoder::reset()
        {
            m_commands.clear();
        }

        // ============================================================================
        // 命令提交
        // ============================================================================

        void CommandEncoder::submitOverlay(PrimitiveType topology,
            uint32_t vertexOffset, uint32_t vertexCount,
            uint32_t zOrder)
        {
            if (vertexCount == 0)
                return;

            DrawCommand cmd;
            cmd.sortKey = buildSortKey(DrawSpace::Overlay, zOrder, topology, 0);
            cmd.space = DrawSpace::Overlay;
            cmd.topology = topology;
            cmd.materialIndex = 0;
            cmd.zOrder = zOrder;
            cmd.overlay.vertexOffset = vertexOffset;
            cmd.overlay.vertexCount = vertexCount;

            m_commands.push_back(cmd);
        }

        void CommandEncoder::submitWorld(PrimitiveType topology, uint16_t materialIndex,
            uint32_t indirectOffset, uint32_t indirectCount,
            uint32_t zOrder)
        {
            if (indirectCount == 0)
                return;

            DrawCommand cmd;
            cmd.sortKey = buildSortKey(DrawSpace::World2D, zOrder, topology, materialIndex);
            cmd.space = DrawSpace::World2D;
            cmd.topology = topology;
            cmd.materialIndex = materialIndex;
            cmd.zOrder = zOrder;
            cmd.world.indirectOffset = indirectOffset;
            cmd.world.indirectCount = indirectCount;

            m_commands.push_back(cmd);
        }

        // ============================================================================
        // 排序键构建
        // ============================================================================

        BatchKey CommandEncoder::buildSortKey(DrawSpace space, uint32_t zOrder,
            PrimitiveType topology, uint16_t materialIndex)
        {
            BatchKey key = 0;
            key |= (static_cast<BatchKey>(space) & 0xFF) << 0;
            key |= (static_cast<BatchKey>(zOrder) & 0xFFFF) << 8;
            key |= (static_cast<BatchKey>(static_cast<uint8_t>(topology)) & 0xFF) << 24;
            key |= (static_cast<BatchKey>(materialIndex) & 0xFFFF) << 32;
            return key;
        }

        // ============================================================================
        // Pipeline 获取
        // ============================================================================

        rhi::PipelineHandle CommandEncoder::getOverlayPipeline(PrimitiveType topology) const
        {
            // overlay 只区分三角形和线段两类
            switch (topology)
            {
                case PrimitiveType::TriangleList:
                case PrimitiveType::TriangleStrip:
                case PrimitiveType::TriangleFan:
                    return m_overlayTriPipeline;
                default:
                    return m_overlayLinePipeline;
            }
        }

        rhi::PipelineHandle CommandEncoder::getWorldPipeline(PrimitiveType topology) const
        {
            uint32_t idx = static_cast<uint32_t>(topology);
            if (idx < PRIMITIVE_TYPE_COUNT)
                return m_worldPipelines[idx];
            return m_worldPipelines[0];
        }

        // ============================================================================
        // 命令执行
        // ============================================================================

        void CommandEncoder::execute(rhi::IDevice* device,
            rhi::BufferHandle worldVB,
            rhi::BufferHandle overlayVB,
            rhi::BufferHandle indirectBuf,
            const float viewMatrix[9])
        {
            if (m_commands.empty())
            {
                m_lastBatchCount = 0;
                return;
            }

            // 按 sortKey 排序，使相同 space/topology/material 的命令连续
            std::sort(m_commands.begin(), m_commands.end(),
                [](const DrawCommand& a, const DrawCommand& b) {
                    return a.sortKey < b.sortKey;
                });

            uint32_t cmdCount = static_cast<uint32_t>(m_commands.size());

            // 计算 batch 数量（按 sortKey 变化点计数）
            m_lastBatchCount = 1;
            for (uint32_t i = 1; i < cmdCount; ++i)
            {
                if (m_commands[i].sortKey != m_commands[i - 1].sortKey)
                    ++m_lastBatchCount;
            }

            SY_DEBUGF("[CommandEncoder] execute: %u commands -> %u batches (DrawBatcher=%s)",
                cmdCount, m_lastBatchCount, m_drawBatcher ? "on" : "off");

            // 当前绑定的状态，用于避免重复绑定
            rhi::PipelineHandle     boundPipeline = {};
            rhi::BufferHandle       boundVB = rhi::NullHandle;
            DrawSpace               boundSpace = DrawSpace::World2D;
            PrimitiveType           boundTopology = PrimitiveType::PointList;
            uint16_t                boundMaterial = 0;
            bool                    stateDirty = true;

            // Phase 8: 如果启用了 DrawBatcher，先重置合批器
            if (m_drawBatcher)
                m_drawBatcher->reset();

            for (uint32_t i = 0; i < cmdCount; ++i)
            {
                const DrawCommand& cmd = m_commands[i];

                // 检测状态变化
                bool spaceChanged = (i == 0) || (cmd.space != boundSpace);
                bool topologyChanged = (i == 0) || (cmd.topology != boundTopology);
                bool materialChanged = (cmd.space == DrawSpace::World2D) &&
                    ((i == 0) || (cmd.materialIndex != boundMaterial));

                if (spaceChanged || topologyChanged)
                {
                    // 选择 pipeline
                    rhi::PipelineHandle pipeline = (cmd.space == DrawSpace::Overlay)
                        ? getOverlayPipeline(cmd.topology)
                        : getWorldPipeline(cmd.topology);

                    if (pipeline != boundPipeline)
                    {
                        if (m_psm)
                            m_psm->bindPipeline(pipeline);
                        else
                            device->bindPipeline(pipeline);
                        boundPipeline = pipeline;
                        device->setUniformMatrix3("uViewMatrix", viewMatrix);
                    }

                    boundSpace = cmd.space;
                    boundTopology = cmd.topology;
                    stateDirty = true;
                }

                if (materialChanged)
                {
                    // TODO: 设置线宽等材质属性
                    boundMaterial = cmd.materialIndex;
                    stateDirty = true;
                }

                // 绑定顶点 buffer（仅在空间变化时，因为 world 和 overlay 用不同 buffer）
                if (spaceChanged || boundVB == rhi::NullHandle)
                {
                    if (cmd.space == DrawSpace::Overlay)
                    {
                        device->bindVertexBuffer(0, overlayVB, 0);
                        boundVB = overlayVB;
                    }
                    else
                    {
                        device->bindVertexBuffer(0, worldVB, 0);
                        boundVB = worldVB;
                    }
                }

                (void)stateDirty; // 保留给未来材质属性切换使用

                // 执行绘制
                if (cmd.space == DrawSpace::Overlay)
                {
                    if (m_drawBatcher)
                    {
                        // Phase 8: 收集 overlay 命令到合批器，跳过立即绘制
                        m_drawBatcher->appendOverlayCmd(boundPipeline,
                            cmd.overlay.vertexOffset,
                            cmd.overlay.vertexCount);
                    }
                    else
                    {
                        device->draw(cmd.overlay.vertexCount, 1,
                            cmd.overlay.vertexOffset, 0);
                    }
                }
                else
                {
                    device->drawIndirect(indirectBuf,
                        cmd.world.indirectOffset,
                        cmd.world.indirectCount,
                        sizeof(DrawIndirectCmd));
                }
            }

            // Phase 8: 统一执行合批后的 overlay 命令（Multi-Draw-Indirect）
            if (m_drawBatcher)
            {
                const auto& groups = m_drawBatcher->build();
                if (!groups.empty())
                {
                    // 确保 overlay 顶点缓冲已绑定
                    device->bindVertexBuffer(0, overlayVB, 0);
                    rhi::BufferHandle mdiBuf = m_drawBatcher->getIndirectBuffer();

                    for (const auto& group : groups)
                    {
                        if (group.pipeline != boundPipeline)
                        {
                            if (m_psm)
                                m_psm->bindPipeline(group.pipeline);
                            else
                                device->bindPipeline(group.pipeline);
                            boundPipeline = group.pipeline;
                        }

                        device->drawIndirect(mdiBuf,
                            group.indirectOffset,
                            group.drawCount,
                            sizeof(DrawIndirectCmd));
                    }

                    SY_DEBUGF("[CommandEncoder] MDI overlay: %u commands -> %u groups",
                        m_drawBatcher->getCommandCount(), m_drawBatcher->getGroupCount());
                }
            }
        }

        // ============================================================================
        // 查询
        // ============================================================================

        uint32_t CommandEncoder::getCommandCount() const
        {
            return static_cast<uint32_t>(m_commands.size());
        }
    } // namespace core
} // namespace render