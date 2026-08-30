/**
 * @file rxSession.cpp
 * @brief Session 实现：一个视口的相机、每帧流程与绘制提交
 *
 * 帧流程（与旧 rendererSession 的差异见各函数注释）：
 *
 *   beginFrame  → Surface::acquireNextImage（GL 后端在此 makeCurrent）
 *                 → device->beginFrame 取命令记录器
 *                 → 瞬态环切段（每 Runtime 每帧只切一次，见 sessionsInFrame）
 *                 → beginRenderPass（clear 在此一次性给定）
 *   submit      → 瞬态刷写 → 按 sortKey 稳定排序 → 逐条绑定并绘制
 *   endFrame    → endRenderPass → device->submitFrame → Surface::present
 *
 * 「提交」与「呈现」分离是多窗口共享资源的前提：N 个 Session 可以先各自
 * 录制并提交，再统一呈现，而不必让设备与某一个窗口绑死。
 */

#include "rt/rxInternal.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace Render::RT::detail
{

    namespace
    {
        /// RHI 结果码 → 公共结果码。两套枚举独立演进，这里是唯一的翻译点。
        RxResult toRxResult(RHI::RhiResult result)
        {
            switch (result)
            {
            case RHI::RhiResult::Ok: return RxResult::Ok;
            case RHI::RhiResult::ErrorInvalidArgument: return RxResult::ErrorInvalidArgument;
            case RHI::RhiResult::ErrorOutOfMemory: return RxResult::ErrorOutOfMemory;
            case RHI::RhiResult::ErrorDeviceLost: return RxResult::ErrorDeviceLost;
            case RHI::RhiResult::ErrorUnsupported: return RxResult::ErrorUnsupportedBackend;
            case RHI::RhiResult::ErrorNotInitialized: return RxResult::ErrorInvalidHandle;
            case RHI::RhiResult::ErrorSurfaceLost: return RxResult::ErrorSurfaceLost;
            case RHI::RhiResult::ErrorSwapchainOutOfDate: return RxResult::ErrorSurfaceOutOfDate;
            case RHI::RhiResult::ErrorShaderCompilation: return RxResult::ErrorShaderCompilation;
            case RHI::RhiResult::ErrorResourceCreation: return RxResult::ErrorOutOfMemory;
            case RHI::RhiResult::ErrorUnknown: return RxResult::ErrorUnknown;
            }
            return RxResult::ErrorUnknown;
        }

        /// 单位矩阵，viewMatrix 未设置时使用（全零矩阵会把所有顶点压到原点）
        const float kIdentity4x4[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        bool isAllZero(const float m[16])
        {
            for (uint32_t i = 0; i < 16; ++i)
            {
                if (m[i] != 0.0f)
                {
                    return false;
                }
            }
            return true;
        }

        /// 每条命令要绘制的图元数量，仅用于统计
        void accumulateTopologyStats(PrimitiveTopology topology, uint32_t elementCount,
                                     uint32_t instanceCount, FrameStats& stats)
        {
            const uint32_t instances = instanceCount == 0 ? 1u : instanceCount;
            switch (topology)
            {
            case PrimitiveTopology::Points:
                stats.pointCount += elementCount * instances;
                break;
            case PrimitiveTopology::Lines:
                stats.lineCount += (elementCount / 2) * instances;
                break;
            case PrimitiveTopology::LineStrip:
            case PrimitiveTopology::LineLoop:
                stats.lineCount += (elementCount > 1 ? elementCount - 1 : 0) * instances;
                break;
            case PrimitiveTopology::Triangles:
                stats.triangleCount += (elementCount / 3) * instances;
                break;
            case PrimitiveTopology::TriangleStrip:
                stats.triangleCount += (elementCount > 2 ? elementCount - 2 : 0) * instances;
                break;
            }
        }
    }  // namespace

    RHI::IndexType toRhiIndexType(IndexType type)
    {
        // IndexType::None 不会走到这里：调用方以 indexCount == 0 表达「非索引绘制」。
        // 注意旧公共头里 IndexType 的数值语义与新 RHI 不同（旧 1 = uint32），
        // 因此不能靠 static_cast 直传，必须显式翻译。
        return type == IndexType::Uint32 ? RHI::IndexType::Uint32 : RHI::IndexType::Uint16;
    }

    // ==================== 生命周期 ====================

    bool Session::create(const SessionDesc& desc)
    {
        runtime = asRuntime(desc.runtime);
        if (!runtime || !runtime->device)
        {
            return false;
        }
        surface = runtime->resolveSurface(desc.surface);
        if (!surface)
        {
            // resolveSurface 已记录具体原因
            return false;
        }
        if (surface->boundSession)
        {
            runtime->log.error("[rt] rxSessionCreate: 该 Surface 已被另一个 Session 绑定。"
                               "两个 Session 画同一个窗口会互相覆盖，请每窗口一个 Session");
            return false;
        }

        std::memcpy(clearColor, desc.clearColor, sizeof(clearColor));
        std::memcpy(viewMatrix, kIdentity4x4, sizeof(viewMatrix));
        stats = FrameStats{};
        inFrame = false;
        cmd = nullptr;
        frameId = 0;
        drawSequence = 0;

        surface->boundSession = this;
        runtime->sessions.push_back(this);
        runtime->log.info("[rt] Session 就绪：surface=%ux%u depth=%s", surface->width, surface->height,
                          surface->hasDepth ? "on" : "off");
        return true;
    }

    void Session::destroy()
    {
        if (!runtime)
        {
            return;
        }
        if (inFrame)
        {
            // 帧未结束就销毁：命令记录器与后备缓冲都还占着，先补上收尾，
            // 否则设备的 beginFrame/submitFrame 配对被破坏，下一帧直接失败。
            runtime->log.error("[rt] rxSessionDestroy: 帧未结束即销毁 Session，已强制收尾");
            endFrame();
        }
        if (surface && surface->boundSession == this)
        {
            surface->boundSession = nullptr;
        }
        auto& list = runtime->sessions;
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
        runtime = nullptr;
        surface = nullptr;
    }

    void Session::setClearColor(float r, float g, float b, float a)
    {
        clearColor[0] = r;
        clearColor[1] = g;
        clearColor[2] = b;
        clearColor[3] = a;
    }

    void Session::setViewMatrix(const float matrix[16])
    {
        if (!matrix)
        {
            return;
        }
        std::memcpy(viewMatrix, matrix, sizeof(viewMatrix));
    }

    void Session::setLighting3D(const Lighting3DDesc* desc)
    {
        if (!desc)
        {
            // 关闭光照：不再上传 FrameUniforms，也不给 3D 管线绑定它。
            // 着色器侧因此不需要「光照是否存在」的分支——网格以材质漫反射色平铺。
            lighting3DEnabled = false;
            frameUniforms.lighting = Lighting3DDesc{};
            return;
        }

        frameUniforms.lighting = *desc;
        lighting3DEnabled = true;
    }

    // ==================== 帧 ====================

    RxResult Session::beginFrame()
    {
        if (!runtime || !runtime->device || !surface || !surface->rhi)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (inFrame)
        {
            runtime->log.error("[rt] rxSessionBeginFrame: 上一帧尚未 EndFrame");
            return RxResult::ErrorUnknown;
        }

        // GL 后端在 acquireNextImage 内部完成 makeCurrent，宿主不需要也不应该自己做。
        const RHI::RhiResult acquired = surface->rhi->acquireNextImage();
        if (acquired != RHI::RhiResult::Ok)
        {
            // OutOfDate 是正常的窗口尺寸变化，调用方 resize 后重试本帧，不记为错误。
            if (acquired != RHI::RhiResult::ErrorSwapchainOutOfDate)
            {
                runtime->log.error("[rt] rxSessionBeginFrame: acquireNextImage 失败（%s）",
                                   RHI::resultName(acquired));
            }
            return toRxResult(acquired);
        }

        cmd = runtime->device->beginFrame(surface->rhi);
        if (!cmd)
        {
            runtime->log.error("[rt] rxSessionBeginFrame: 设备未返回命令记录器");
            return RxResult::ErrorDeviceLost;
        }

        // 瞬态环按 Runtime 计数切段，而不是按 Session：多窗口共享同一个环，
        // 若每个 Session 都切一次，第二个窗口开帧就会把第一个窗口本帧的
        // 顶点数据所在段翻掉，表现为「某个窗口随机缺图元」。
        if (runtime->sessionsInFrame == 0)
        {
            runtime->transient.beginFrame();
        }
        runtime->sessionsInFrame += 1;

        // 窗口尺寸以 Surface 的实际交换链尺寸为准：宿主可能漏调 rxSurfaceResize。
        const RHI::Extent2D extent = surface->rhi->extent();
        surface->width = extent.width;
        surface->height = extent.height;

        RHI::RenderPassBeginDesc pass{};
        pass.colorAttachmentCount = 1;
        // 无效纹理句柄 = 使用交换链当前后备缓冲
        pass.colorAttachments[0].texture = {};
        pass.colorAttachments[0].loadOp = RHI::LoadOp::Clear;
        pass.colorAttachments[0].storeOp = RHI::StoreOp::Store;
        pass.colorAttachments[0].clearValue = { clearColor[0], clearColor[1], clearColor[2],
                                                clearColor[3] };
        pass.hasDepthAttachment = surface->hasDepth;
        if (surface->hasDepth)
        {
            pass.depthAttachment.texture = surface->rhi->depthTexture();
            pass.depthAttachment.loadOp = RHI::LoadOp::Clear;
            pass.depthAttachment.storeOp = RHI::StoreOp::DontCare;
            pass.depthAttachment.clearDepth = 1.0f;
        }
        pass.extent = extent;
        pass.debugName = "RxSessionPass";

        const RHI::RhiResult began = cmd->beginRenderPass(pass);
        if (began != RHI::RhiResult::Ok)
        {
            runtime->log.error("[rt] rxSessionBeginFrame: beginRenderPass 失败（%s）",
                               RHI::resultName(began));
            // 设备帧已经开了，必须配对提交，否则后续所有帧都会被判为未配对。
            runtime->device->submitFrame();
            runtime->sessionsInFrame -= 1;
            cmd = nullptr;
            return toRxResult(began);
        }

        RHI::Viewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        cmd->setViewport(viewport);

        stats = FrameStats{};
        drawSequence = 0;
        inFrame = true;
        return RxResult::Ok;
    }

    RxResult Session::allocTransient(uint64_t sizeBytes, TransientAlloc* out)
    {
        if (!out || sizeBytes == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        if (!runtime)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (!inFrame)
        {
            runtime->log.error("[rt] rxSessionAllocTransient: 必须在 BeginFrame/EndFrame 之间调用");
            return RxResult::ErrorUnknown;
        }
        if (!runtime->transient.allocate(sizeBytes, out))
        {
            return RxResult::ErrorOutOfMemory;
        }
        return RxResult::Ok;
    }

    bool Session::prepareSubmitState(const float* packetViewMatrix, const float* packetViewport,
                                     PushConstants* out)
    {
        // 视口：packet.viewport 优先（分屏/子视口场景），否则整个表面
        const bool hasPacketViewport =
            packetViewport != nullptr && packetViewport[2] > 0.0f && packetViewport[3] > 0.0f;
        const float viewportW =
            hasPacketViewport ? packetViewport[2] : static_cast<float>(surface->width);
        const float viewportH =
            hasPacketViewport ? packetViewport[3] : static_cast<float>(surface->height);
        if (hasPacketViewport)
        {
            RHI::Viewport viewport{};
            viewport.x = packetViewport[0];
            viewport.y = packetViewport[1];
            viewport.width = viewportW;
            viewport.height = viewportH;
            cmd->setViewport(viewport);
        }

        // 视图矩阵：packet 内的为本次提交的权威值；全零表示「沿用 Session 的」，
        // 这样只用 rxSessionSetViewMatrix 的调用方无需每次填 packet。
        if (packetViewMatrix == nullptr || isAllZero(packetViewMatrix))
        {
            std::memcpy(out->view, viewMatrix, sizeof(out->view));
        }
        else
        {
            std::memcpy(out->view, packetViewMatrix, sizeof(out->view));
            std::memcpy(viewMatrix, packetViewMatrix, sizeof(viewMatrix));
        }
        out->viewport[0] = viewportW;
        out->viewport[1] = viewportH;
        return hasPacketViewport;
    }

    void Session::recordCommands(const DrawCommand* commands, const uint32_t* order, uint32_t count,
                                 PushConstants& push)
    {
        uint16_t boundPipeline = 0;
        BufferHandle boundVertexBuffer = BufferHandle::Invalid;
        uint64_t boundVertexOffset = UINT64_MAX;
        TextureHandle boundTexture = TextureHandle::Invalid;
        /// 光照绑定组本帧是否已绑到当前管线上。换管线会失效（见下）。
        bool boundLighting = false;
        PushConstants pushed{};
        bool pushedValid = false;

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t index = order ? order[i] : i;
            const DrawCommand& command = commands[index];
            if (command.vertexCount == 0 && command.indexCount == 0)
            {
                continue;
            }

            const RHI::BufferHandle vertexBuffer = runtime->resolveBuffer(command.vertexBuffer);
            if (!vertexBuffer.valid())
            {
                runtime->log.warn("[rt] 第 %u 条命令的顶点缓冲句柄无效，已跳过", index);
                continue;
            }

            // 材质提供缺省线宽/点大小，命令里的非零值覆盖它
            const MaterialDesc* material = command.materialIndex != 0 &&
                                                   command.materialIndex < runtime->materials.size()
                                               ? &runtime->materials[command.materialIndex]
                                               : nullptr;
            float lineWidth = command.lineWidth > 0.0f
                                  ? command.lineWidth
                                  : (material ? material->lineWidth : 1.0f);
            const float pointSize = command.pointSize > 0.0f
                                        ? command.pointSize
                                        : (material ? material->pointSize : 1.0f);
            if (lineWidth <= 0.0f)
            {
                lineWidth = 1.0f;
            }

            // 线宽属于管线固定状态（Vulkan/Metal 都不能在录制期改），
            // 因此不同线宽必须落到不同管线。量化避免浮点抖动炸开管线数量。
            uint16_t pipelineIndex = command.pipelineIndex;
            if (pipelineIndex == 0)
            {
                pipelineIndex = runtime->resolvePipeline(command.vertexFormat, command.space,
                                                         command.topology, lineWidth);
            }
            const RHI::PipelineHandle pipeline = runtime->rhiPipeline(pipelineIndex);
            if (!pipeline.valid())
            {
                runtime->log.warn("[rt] 第 %u 条命令没有可用管线（fmt=%d space=%d topo=%d），已跳过",
                                  index, static_cast<int>(command.vertexFormat),
                                  static_cast<int>(command.space),
                                  static_cast<int>(command.topology));
                continue;
            }

            if (pipelineIndex != boundPipeline)
            {
                cmd->bindPipeline(pipeline);
                boundPipeline = pipelineIndex;
                stats.pipelineSwitches += 1;
                // 换管线后 pushConstant 与绑定组的有效性由后端决定，
                // 统一重推一次比逐后端推理更可靠。
                pushedValid = false;
                boundVertexBuffer = BufferHandle::Invalid;
                boundVertexOffset = UINT64_MAX;
                boundTexture = TextureHandle::Invalid;
                boundLighting = false;
            }

            // 3D 管线的光照绑定组：只有声明了 FrameUniforms 块的管线才绑，
            // 否则 GL 后端会对每条 2D 命令 warn 一次「未声明 binding 1」。
            if (!boundLighting && runtime->pipelineNeedsLighting3D(pipelineIndex))
            {
                if (runtime->lightingBindGroup.valid())
                {
                    cmd->bindBindGroup(0, runtime->lightingBindGroup);
                    boundLighting = true;
                }
                else
                {
                    runtime->log.warn("[rt] 3D pipeline bound but lighting uniforms are unavailable, "
                                      "mesh will render unlit");
                }
            }

            push.pointSize = pointSize;
            // 3D 材质段：无材质时显式复位，否则上一条命令的颜色会泄漏到这条
            // ——同一帧里 2D 命令不读这段，但相邻的两个 3D 网格会串色。
            if (material)
            {
                std::memcpy(push.matDiffuse, material->color, sizeof(push.matDiffuse));
                std::memcpy(push.matAmbient, material->ambient, sizeof(push.matAmbient));
                std::memcpy(push.matSpecular, material->specular, sizeof(push.matSpecular));
                push.matShininess = material->shininess > 0.0f ? material->shininess : 1.0f;
            }
            else
            {
                const PushConstants defaults{};
                std::memcpy(push.matDiffuse, defaults.matDiffuse, sizeof(push.matDiffuse));
                std::memcpy(push.matAmbient, defaults.matAmbient, sizeof(push.matAmbient));
                std::memcpy(push.matSpecular, defaults.matSpecular, sizeof(push.matSpecular));
                push.matShininess = defaults.matShininess;
            }
            if (!pushedValid || std::memcmp(&pushed, &push, sizeof(push)) != 0)
            {
                cmd->pushConstants(0, kPushConstantBytes, &push);
                pushed = push;
                pushedValid = true;
            }

            // vertexOffset / indexOffset 是**字节**偏移（与 TransientAlloc::offset
            // 同一坐标系），因此直接作为绑定偏移使用，draw 的 firstVertex 恒为 0。
            if (command.vertexBuffer != boundVertexBuffer || command.vertexOffset != boundVertexOffset)
            {
                cmd->bindVertexBuffer(0, vertexBuffer, command.vertexOffset);
                boundVertexBuffer = command.vertexBuffer;
                boundVertexOffset = command.vertexOffset;
            }

            if (command.texture != TextureHandle::Invalid && command.texture != boundTexture)
            {
                const RHI::BindGroupHandle group = runtime->bindGroupForTexture(command.texture);
                if (group.valid())
                {
                    cmd->bindBindGroup(0, group);
                    boundTexture = command.texture;
                }
                else
                {
                    runtime->log.warn("[rt] 第 %u 条命令的纹理句柄无效", index);
                }
            }

            const uint32_t instanceCount = command.instanceCount == 0 ? 1u : command.instanceCount;
            const bool indexed = command.indexCount > 0 && command.indexType != IndexType::None;

            if (indexed)
            {
                const RHI::BufferHandle indexBuffer = runtime->resolveBuffer(command.indexBuffer);
                if (!indexBuffer.valid())
                {
                    runtime->log.warn("[rt] 第 %u 条命令声明了索引绘制但索引缓冲句柄无效，已跳过",
                                      index);
                    continue;
                }
                cmd->bindIndexBuffer(indexBuffer, command.indexOffset,
                                     toRhiIndexType(command.indexType));
                cmd->drawIndexed(command.indexCount, instanceCount, 0, 0, command.firstInstance);
                accumulateTopologyStats(command.topology, command.indexCount, instanceCount, stats);
            }
            else
            {
                cmd->draw(command.vertexCount, instanceCount, 0, command.firstInstance);
                accumulateTopologyStats(command.topology, command.vertexCount, instanceCount, stats);
            }

            stats.drawCallCount += 1;
            drawSequence += 1;
        }
    }

    RxResult Session::submit(const DrawPacket& packet)
    {
        if (!runtime || !surface)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (!inFrame || !cmd)
        {
            runtime->log.error("[rt] rxSessionSubmit: 必须在 BeginFrame/EndFrame 之间调用");
            return RxResult::ErrorUnknown;
        }
        if (packet.commandCount == 0)
        {
            return RxResult::Ok;
        }
        if (!packet.commands)
        {
            return RxResult::ErrorInvalidArgument;
        }

        // 顶点数据必须在任何绘制之前落到 GPU。旧实现把上传散在
        // 每条命令旁边，导致同一段瞬态内存可能在被引用后又被写。
        runtime->transient.flush();
        // 几何仓与瞬态环可以混用（常驻图元 + 预览线在同一帧），
        // 因此这条路径也要先把仓的脏区提交。
        stats.geometryUploadBytes += runtime->flushGeometryStores();

        PushConstants push{};
        const bool restoreViewport = prepareSubmitState(packet.viewMatrix, packet.viewport, &push);

        // 按 sortKey 稳定排序：同键命令保持提交顺序，覆盖层的叠放才可预测。
        sortScratch.resize(packet.commandCount);
        for (uint32_t i = 0; i < packet.commandCount; ++i)
        {
            sortScratch[i] = i;
        }
        const DrawCommand* commands = packet.commands;
        std::stable_sort(sortScratch.begin(), sortScratch.end(),
                         [commands](uint32_t a, uint32_t b) {
                             return commands[a].sortKey < commands[b].sortKey;
                         });

        recordCommands(commands, sortScratch.data(), packet.commandCount, push);

        if (restoreViewport)
        {
            // 恢复整表面视口，避免影响同帧后续的 Submit
            RHI::Viewport full{};
            full.width = static_cast<float>(surface->width);
            full.height = static_cast<float>(surface->height);
            cmd->setViewport(full);
        }
        return RxResult::Ok;
    }

    RxResult Session::submitDrawList(DrawList* list, const float viewBounds[4])
    {
        if (!runtime || !surface)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (!list)
        {
            return RxResult::ErrorInvalidArgument;
        }
        if (!inFrame || !cmd)
        {
            runtime->log.error("[rt] rxSessionSubmitDrawList: 必须在 BeginFrame/EndFrame 之间调用");
            return RxResult::ErrorUnknown;
        }

        // 常驻几何的脏区在这里一次性提交；瞬态环也要刷，因为同一帧里
        // 覆盖层仍可能走瞬态路径。
        runtime->transient.flush();
        stats.geometryUploadBytes += runtime->flushGeometryStores();

        PushConstants push{};
        const bool restoreViewport = prepareSubmitState(nullptr, nullptr, &push);

        uint32_t culled = 0;
        uint32_t merged = 0;
        const std::vector<DrawCommand>& resolved = list->resolve(viewBounds, culled, merged);
        stats.culledCommandCount += culled;
        stats.mergedDrawCount += merged;

        // resolve 已按 sortKey 排好序并完成合批，这里不需要再排一次——
        // 「每帧不重排」正是保留式列表相对 DrawPacket 的收益所在。
        recordCommands(resolved.data(), nullptr, static_cast<uint32_t>(resolved.size()), push);

        if (restoreViewport)
        {
            RHI::Viewport full{};
            full.width = static_cast<float>(surface->width);
            full.height = static_cast<float>(surface->height);
            cmd->setViewport(full);
        }
        return RxResult::Ok;
    }


    RxResult Session::endFrame()
    {
        if (!runtime || !runtime->device)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (!inFrame)
        {
            runtime->log.error("[rt] rxSessionEndFrame: 本帧未 BeginFrame");
            return RxResult::ErrorUnknown;
        }

        if (cmd)
        {
            cmd->endRenderPass();
        }
        const RHI::RhiResult submitted = runtime->device->submitFrame();

        stats.transientBytesUsed = runtime->transient.usedBytesThisFrame();
        stats.gpuMemoryBytes = runtime->device->gpuMemoryUsageBytes();

        // 呈现与提交分离：多窗口时可以先各自 submitFrame，再统一 present。
        RHI::RhiResult presented = RHI::RhiResult::Ok;
        if (surface && surface->rhi)
        {
            presented = surface->rhi->present();
        }

        inFrame = false;
        cmd = nullptr;
        frameId += 1;
        if (runtime->sessionsInFrame > 0)
        {
            runtime->sessionsInFrame -= 1;
        }

        if (submitted != RHI::RhiResult::Ok)
        {
            runtime->log.error("[rt] rxSessionEndFrame: submitFrame 失败（%s）",
                               RHI::resultName(submitted));
            return toRxResult(submitted);
        }
        if (presented != RHI::RhiResult::Ok)
        {
            if (presented != RHI::RhiResult::ErrorSwapchainOutOfDate)
            {
                runtime->log.error("[rt] rxSessionEndFrame: present 失败（%s）",
                                   RHI::resultName(presented));
            }
            return toRxResult(presented);
        }
        return RxResult::Ok;
    }

    RxResult Session::readPixels(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                 void* outBytes, uint64_t outByteCapacity)
    {
        if (!runtime || !runtime->device || !surface || !surface->rhi)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (!outBytes || width == 0 || height == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        if (!inFrame)
        {
            // EndFrame 之后后备缓冲已交给呈现，内容不再保证有效。
            // 这里报错而不是「尽力读一次」：读到上一帧或空白画面
            // 比明确失败更难排查。
            runtime->log.error("[rt] rxSessionReadPixels: 必须在 EndFrame 之前调用");
            return RxResult::ErrorUnknown;
        }

        constexpr uint64_t kBytesPerPixel = 4;
        const uint64_t required = static_cast<uint64_t>(width) * height * kBytesPerPixel;
        if (outByteCapacity < required)
        {
            runtime->log.error("[rt] rxSessionReadPixels: 输出缓冲不足（需要 %llu，给了 %llu）",
                               static_cast<unsigned long long>(required),
                               static_cast<unsigned long long>(outByteCapacity));
            return RxResult::ErrorInvalidArgument;
        }

        const RHI::TextureHandle color = surface->rhi->currentColorTexture();
        if (!color.valid())
        {
            runtime->log.error("[rt] rxSessionReadPixels: 表面未提供颜色附件句柄");
            return RxResult::ErrorUnsupportedBackend;
        }

        // 已录制的绘制必须先落到附件上才能读到。GL 下 readTexture 内部
        // 会做同步读回，但命令仍可能停在驱动队列里。
        if (cmd)
        {
            cmd->endRenderPass();
        }
        runtime->device->waitIdle();

        RHI::Rect2D region{};
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;

        uint32_t rowPitch = 0;
        const RHI::RhiResult read =
            runtime->device->readTexture(color, region, outBytes, outByteCapacity, &rowPitch);

        // 读回后必须重开 RenderPass：EndFrame 会无条件 endRenderPass，
        // 不重开就变成未配对的 end，后端会报错并把整帧判废。
        if (cmd)
        {
        // 3D 光照参数上传。必须在 beginRenderPass 之前：Vulkan 不允许在
        // RenderPass 内做缓冲拷贝。每帧无条件重传而不是「脏了才传」——
        // 光照 UBO 由整个 Runtime 共享，多窗口下每个 Session 的参数不同，
        // 按脏标记跳过会让第二个窗口沿用第一个窗口的光照。
        if (lighting3DEnabled)
        {
            runtime->uploadLighting3D(frameUniforms);
        }

        RHI::RenderPassBeginDesc pass{};
            pass.colorAttachmentCount = 1;
            pass.colorAttachments[0].texture = {};
            // Load 而不是 Clear：本帧已画好的内容不能被清掉
            pass.colorAttachments[0].loadOp = RHI::LoadOp::Load;
            pass.colorAttachments[0].storeOp = RHI::StoreOp::Store;
            pass.hasDepthAttachment = surface->hasDepth;
            if (surface->hasDepth)
            {
                pass.depthAttachment.texture = surface->rhi->depthTexture();
                pass.depthAttachment.loadOp = RHI::LoadOp::Load;
                pass.depthAttachment.storeOp = RHI::StoreOp::DontCare;
            }
            pass.extent = surface->rhi->extent();
            pass.debugName = "RxSessionPassAfterReadback";
            cmd->beginRenderPass(pass);
        }

        if (read != RHI::RhiResult::Ok)
        {
            runtime->log.error("[rt] rxSessionReadPixels: readTexture 失败（%s）",
                               RHI::resultName(read));
            return toRxResult(read);
        }

        // 契约是「恒 RGBA8、左上原点、rowPitch = width * 4」。
        // 后端返回的行距不同（对齐）时按契约压紧，交换链是 BGRA 时换通道，
        // 否则调用方要为每个后端各写一份解释代码。
        const uint32_t tight = width * static_cast<uint32_t>(kBytesPerPixel);
        auto* bytes = static_cast<uint8_t*>(outBytes);
        if (rowPitch != 0 && rowPitch != tight)
        {
            for (uint32_t row = 1; row < height; ++row)
            {
                std::memmove(bytes + static_cast<size_t>(row) * tight,
                             bytes + static_cast<size_t>(row) * rowPitch, tight);
            }
        }

        const RHI::Format format = surface->rhi->colorFormat();
        if (format == RHI::Format::BGRA8Unorm || format == RHI::Format::BGRA8Srgb)
        {
            for (uint64_t i = 0; i < required; i += kBytesPerPixel)
            {
                std::swap(bytes[i], bytes[i + 2]);
            }
        }
        return RxResult::Ok;
    }

    RxResult Session::queryVisibility(const float* aabbs, uint32_t aabbCount,
                                      const float viewBounds[4], VisibilityResult* out)
    {
        if (!out || !out->indices || !aabbs || !viewBounds)
        {
            return RxResult::ErrorInvalidArgument;
        }
        out->count = 0;
        if (aabbCount == 0 || out->capacity == 0)
        {
            return RxResult::Ok;
        }

        // aabbs 为紧凑排列的 (minX, minY, maxX, maxY)；viewBounds 同布局。
        // 纯 CPU 的矩形相交，不涉及 GPU——放在 DLL 内只是为了让调用方
        // 少写一份公式，语义上它不属于渲染。
        const float viewMinX = viewBounds[0];
        const float viewMinY = viewBounds[1];
        const float viewMaxX = viewBounds[2];
        const float viewMaxY = viewBounds[3];

        for (uint32_t i = 0; i < aabbCount; ++i)
        {
            const float* box = aabbs + static_cast<size_t>(i) * 4;
            const bool disjoint =
                box[2] < viewMinX || box[0] > viewMaxX || box[3] < viewMinY || box[1] > viewMaxY;
            if (disjoint)
            {
                continue;
            }
            if (out->count >= out->capacity)
            {
                // 容量不足不是错误：调用方按 count == capacity 判断是否需要扩容重试。
                if (runtime)
                {
                    runtime->log.warn("[rt] rxSessionQueryVisibility: 输出容量 %u 已满，结果被截断",
                                      out->capacity);
                }
                break;
            }
            out->indices[out->count] = i;
            out->count += 1;
        }
        return RxResult::Ok;
    }

}  // namespace Render::RT::detail
