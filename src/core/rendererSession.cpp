#include "rendererSession.h"
#include "Log/SyLogger.h"

#include <algorithm>

namespace Render
{
    namespace RT
    {

        Session::Session() = default;
        Session::~Session() { destroy(); }

        bool Session::create(const SessionDesc* desc)
        {
            if (!desc)
                return false;
            m_rt = reinterpret_cast<Runtime*>(static_cast<uintptr_t>(desc->runtime));
            if (!m_rt)
                return false;
            m_width = desc->width > 0 ? desc->width : 1;
            m_height = desc->height > 0 ? desc->height : 1;
            m_nativeWindow = desc->nativeWindowHandle;
            std::memcpy(m_clearColor, desc->clearColor, sizeof(m_clearColor));
            return true;
        }

        void Session::destroy() { m_rt = nullptr; }

        void Session::resize(uint32_t width, uint32_t height)
        {
            m_width = width > 0 ? width : 1;
            m_height = height > 0 ? height : 1;
        }

        void Session::setClearColor(float r, float g, float b, float a)
        {
            m_clearColor[0] = r;
            m_clearColor[1] = g;
            m_clearColor[2] = b;
            m_clearColor[3] = a;
        }

        void Session::setViewMatrix(const float viewMatrix[16])
        {
            if (viewMatrix)
                std::memcpy(m_viewMatrix, viewMatrix, sizeof(m_viewMatrix));
        }

        uint32_t Session::vertexStride(uint8_t fmt)
        {
            switch (static_cast<RTVertexFormat>(fmt))
            {
            case RTVertexFormat::P3C3: return 24;
            case RTVertexFormat::P3C4: return 28;
            case RTVertexFormat::P3N3: return 24;
            case RTVertexFormat::P2T2C4: return 32;
            default: return 24;
            }
        }

        uint32_t Session::indexStride(uint8_t type)
        {
            return type == 1 ? 4u : 2u;
        }

        void Session::drawCommand(const RTDrawCommand& cmd)
        {
            RHI::IDevice* dev = m_rt->device();
            if (!dev)
                return;

            // 优先按命令自身的 topology/vertexFormat/space 解析管线，使 RTDrawCommand.topology
            // 真正生效（默认管线拓扑是固定的，无法覆盖每笔绘制）。退化时回退到索引默认管线。
            RHI::PipelineHandle ph = m_rt->resolvePipeline(cmd);
            if (ph == RHI::NullHandle)
                ph = m_rt->rhiPipeline(cmd.pipelineIndex);
            if (ph == RHI::NullHandle)
                return;

            dev->bindPipeline(ph);

            RHI::BufferHandle vb = m_rt->rhiBuffer(cmd.vertexBufferHandle);
            if (vb == RHI::NullHandle)
                return;
            uint32_t vstride = vertexStride(cmd.vertexFormat);
            dev->bindVertexBuffer(0, vb, static_cast<uint64_t>(cmd.vertexOffset) * vstride);

            if (cmd.textureHandle != 0)
            {
                RHI::TextureHandle th = m_rt->rhiTexture(cmd.textureHandle);
                if (th != RHI::NullHandle)
                    dev->bindTexture(0, 0, th);
            }

            if (cmd.space == RenderSpace::Screen)
                dev->setUniformVec2("uViewport", m_viewport);
            else
                dev->setUniformMatrix4("uView", m_viewMatrix);

            if (cmd.lineWidth > 0.0f)
                dev->setLineWidth(cmd.lineWidth);
            if (cmd.pointSize > 0.0f)
                dev->setUniformFloat("uPointSize", cmd.pointSize);

            if (cmd.indexBufferHandle != 0)
            {
                RHI::BufferHandle ib = m_rt->rhiBuffer(cmd.indexBufferHandle);
                if (ib != RHI::NullHandle)
                {
                    dev->bindIndexBuffer(ib, static_cast<uint64_t>(cmd.indexOffset) * indexStride(cmd.indexType));
                    dev->drawIndexed(cmd.indexCount, cmd.instanceCount, 0, static_cast<int32_t>(cmd.vertexOffset),
                        cmd.firstInstance);
                    m_triangles += cmd.indexCount / 3 * cmd.instanceCount;
                }
            }
            else
            {
                dev->draw(cmd.vertexCount, cmd.instanceCount, cmd.vertexOffset, cmd.firstInstance);
                switch (cmd.topology)
                {
                case RTPrimitiveTopology::Points: m_points += cmd.vertexCount * cmd.instanceCount; break;
                case RTPrimitiveTopology::Triangles:
                case RTPrimitiveTopology::TriangleStrip:
                case RTPrimitiveTopology::TriangleFan: m_triangles += cmd.vertexCount * cmd.instanceCount; break;
                default: m_lines += cmd.vertexCount * cmd.instanceCount; break;
                }
            }
            ++m_drawCalls;
        }

        void Session::submitDrawCommands(const RTDrawPacket* packet)
        {
            if (!packet || !m_rt || !m_rt->device())
                return;

            RHI::IDevice* dev = m_rt->device();
            // 共享设备模式下（existingDevice），帧的 begin/clear/present 由外部（旧 renderFrame）负责，
            // 这里只负责绘制命令，避免重复 beginFrame/present 冲突。
            if (m_rt->ownsDevice())
            {
                dev->beginFrame();
                dev->setClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
                dev->enableDepthTest(false);
                dev->enableBlend(true);
                dev->clear(0x0001 | 0x0002);
            }

            if (packet->viewport[2] > 0.0f && packet->viewport[3] > 0.0f)
                std::memcpy(m_viewport, packet->viewport, sizeof(m_viewport));
            else
            {
                m_viewport[0] = 0;
                m_viewport[1] = 0;
                m_viewport[2] = static_cast<float>(m_width);
                m_viewport[3] = static_cast<float>(m_height);
            }

            std::memcpy(m_viewMatrix, packet->viewMatrix, sizeof(m_viewMatrix));

            RHI::Viewport vp{};
            vp.x = m_viewport[0];
            vp.y = m_viewport[1];
            vp.w = m_viewport[2];
            vp.h = m_viewport[3];
            dev->setViewport(vp);

            // 将 CPU 暂存缓冲中的瞬态顶点数据推送到 GPU 缓冲（兼容无持久映射的平台）。
            const uint8_t* staged = m_rt->transientStagingData();
            const uint64_t used = m_rt->transientCursor();
            if (staged && used > 0)
            {
                RHI::BufferHandle tb = m_rt->rhiBuffer(m_rt->transientBufferId());
                if (tb != RHI::NullHandle)
                    dev->uploadBuffer(tb, 0, used, staged);
            }

            m_drawCalls = 0;
            m_triangles = 0;
            m_lines = 0;
            m_points = 0;

            const RTDrawCommand* cmds = packet->commands;
            uint32_t count = packet->commandCount;
            if (!cmds || count == 0)
            {
                return;
            }

            std::vector<uint32_t> order(count);
            for (uint32_t i = 0; i < count; ++i)
                order[i] = i;

            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                return cmds[a].sortKey < cmds[b].sortKey;
            });

            for (uint32_t i : order)
                drawCommand(cmds[i]);
        }

        void Session::queryVisibility(const float* aabbs, uint32_t aabbCount, const float viewBounds[4],
            RTVisibilityResult* out) const
        {
            if (!out || !out->indices || out->maxCount == 0)
                return;
            out->count = 0;
            if (!aabbs || aabbCount == 0 || !viewBounds)
                return;
            for (uint32_t i = 0; i < aabbCount && out->count < out->maxCount; ++i)
            {
                const float* b = aabbs + i * 4;
                bool inside = !(b[2] < viewBounds[0] || b[0] > viewBounds[2] || b[3] < viewBounds[1] ||
                                b[1] > viewBounds[3]);
                if (inside)
                    out->indices[out->count++] = i;
            }
        }

        void Session::present()
        {
            if (!m_rt || !m_rt->device())
                return;
            // 共享设备模式下帧的 present 由外部负责，这里跳过。
            if (!m_rt->ownsDevice())
                return;
            m_rt->device()->endFrame();
            m_rt->device()->present();
        }

        void Session::getStats(RTStats* out) const
        {
            if (!out)
                return;
            out->drawCallCount = m_drawCalls;
            out->triangleCount = m_triangles;
            out->lineCount = m_lines;
            out->pointCount = m_points;
            out->gpuMemoryBytes = m_rt ? m_rt->gpuMemoryBytes() : 0;
        }

    }  // namespace RT
}  // namespace Render
