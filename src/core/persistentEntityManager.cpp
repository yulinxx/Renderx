/**
 * @file persistent_entity_manager.cpp
 * @brief 持久图元管理器实现
 */
#include "persistentEntityManager.h"
#include "Log/SyLogger.h"
#include <algorithm>
#include <cstring>

namespace Render
{
    namespace core
    {
        bool PersistentEntityManager::initialize(RHI::IDevice* device, uint32_t maxEntities)
        {
            if (m_initialized || !device || maxEntities == 0)
            {
                return false;
            }

            m_device = device;
            m_maxEntities = maxEntities;
            m_entityCount = 0;

            m_entities.reserve(maxEntities);
            m_dirtyFlags.reserve(maxEntities);

            // ------------------------------------------------------------------------
            // 创建图元元数据 SSBO
            // ------------------------------------------------------------------------
            {
                RHI::BufferDesc desc;
                desc.size = maxEntities * sizeof(EntityGpuData);
                desc.usage = RHI::BufferUsage::ShaderStorage;
                desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_SSBO";
                m_entityBuffer = device->createBuffer(desc);

                if (m_entityBuffer == RHI::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create entity buffer");
                    return false;
                }
            }

            // ------------------------------------------------------------------------
            // 创建可见性结果缓冲（每个图元一个 uint32：0=不可见 1=可见）
            // ------------------------------------------------------------------------
            {
                RHI::BufferDesc desc;
                desc.size = maxEntities * sizeof(uint32_t);
                desc.usage = RHI::BufferUsage::ShaderStorage;
                desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Visibility";

                m_visibilityBuffer = device->createBuffer(desc);
                if (m_visibilityBuffer == RHI::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create visibility buffer");
                    return false;
                }

                // 初始化为全 0
                std::vector<uint32_t> zeros(maxEntities, 0);
                device->uploadBuffer(m_visibilityBuffer, 0, maxEntities * sizeof(uint32_t), zeros.data());
            }

            // ------------------------------------------------------------------------
            // 创建 indirect draw 缓冲
            // ------------------------------------------------------------------------
            {
                RHI::BufferDesc desc;
                desc.size = maxEntities * sizeof(DrawIndirectCmd);
                desc.usage = RHI::BufferUsage::Indirect;
                desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Indirect";
                m_indirectBuffer = device->createBuffer(desc);
                if (m_indirectBuffer == RHI::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create indirect buffer");
                    return false;
                }
            }

            // ------------------------------------------------------------------------
            // 创建原子计数缓冲（用于 compute shader 写入 indirect 命令数量）
            // ------------------------------------------------------------------------
            {
                RHI::BufferDesc desc;
                desc.size = sizeof(uint32_t);
                desc.usage = RHI::BufferUsage::ShaderStorage;
                desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Count";

                m_countBuffer = device->createBuffer(desc);
                if (m_countBuffer == RHI::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create count buffer");
                    return false;
                }

                uint32_t zero = 0;
                device->uploadBuffer(m_countBuffer, 0, sizeof(uint32_t), &zero);
            }

            if (!ensureCullingPipeline())
            {
                return false;
            }

            m_initialized = true;

            SY_DEBUGF("[PersistentEntityManager] initialized, maxEntities=%u", maxEntities);

            return true;
        }

        void PersistentEntityManager::shutdown()
        {
            if (!m_initialized || !m_device)
            {
                return;
            }

            if (m_entityBuffer != RHI::NullHandle)
            {
                m_device->destroyBuffer(m_entityBuffer);
                m_entityBuffer = RHI::NullHandle;
            }

            if (m_visibilityBuffer != RHI::NullHandle)
            {
                m_device->destroyBuffer(m_visibilityBuffer);
                m_visibilityBuffer = RHI::NullHandle;
            }

            if (m_indirectBuffer != RHI::NullHandle)
            {
                m_device->destroyBuffer(m_indirectBuffer);
                m_indirectBuffer = RHI::NullHandle;
            }

            if (m_countBuffer != RHI::NullHandle)
            {
                m_device->destroyBuffer(m_countBuffer);
                m_countBuffer = RHI::NullHandle;
            }

            if (m_cullingPipeline != RHI::NullHandle)
            {
                m_device->destroyPipeline(m_cullingPipeline);
                m_cullingPipeline = RHI::NullHandle;
            }

            m_entities.clear();
            m_entities.shrink_to_fit();
            m_dirtyFlags.clear();
            m_dirtyFlags.shrink_to_fit();

            m_entityCount = 0;
            m_maxEntities = 0;
            m_initialized = false;
            m_device = nullptr;

            SY_DEBUG("[PersistentEntityManager] shutdown");
        }

        uint32_t PersistentEntityManager::addEntity(const PersistentEntity& entity, uint32_t renderWorldIndex)
        {
            if (!m_initialized)
            {
                SY_ERROR("[PersistentEntityManager] addEntity: not initialized");
                return UINT32_MAX;
            }

            if (m_entityCount >= m_maxEntities)
            {
                SY_ERROR("[PersistentEntityManager] addEntity: max capacity reached");
                return UINT32_MAX;
            }

            uint32_t index = m_entityCount++;
            if (index < m_entities.size())
            {
                m_entities[index] = entity;
                m_dirtyFlags[index] = true;
            }
            else
            {
                m_entities.push_back(entity);
                m_dirtyFlags.push_back(true);
            }

            // 记录 PEM 索引 -> RenderWorld 索引的映射
            if (renderWorldIndex != UINT32_MAX)
            {
                if (index < m_pemToRenderWorldIndex.size())
                {
                    m_pemToRenderWorldIndex[index] = renderWorldIndex;
                }
                else
                {
                    m_pemToRenderWorldIndex.push_back(renderWorldIndex);
                }
            }
            else
            {
                // 兼容旧调用：以 entity.id 作为回退（不准确但能工作）
                if (index < m_pemToRenderWorldIndex.size())
                {
                    m_pemToRenderWorldIndex[index] = entity.id;
                }
                else
                {
                    m_pemToRenderWorldIndex.push_back(entity.id);
                }
            }

            // SY_DEBUGF("[PersistentEntityManager] addEntity id=%u at index=%u (rwIdx=%u)", entity.id, index,
            // renderWorldIndex);

            return index;
        }

        void PersistentEntityManager::removeEntity(uint32_t index)
        {
            if (!m_initialized || index >= m_entities.size())
            {
                return;
            }

            // 惰性删除：标记为无效，vertexCount=0 表示该图元不参与绘制
            m_entities[index].vertexCount = 0;
            m_entities[index].flags = 0;
            m_dirtyFlags[index] = true;
        }

        void PersistentEntityManager::clearEntities()
        {
            if (!m_initialized)
            {
                return;
            }

            m_entityCount = 0;
            m_entities.clear();
            m_pemToRenderWorldIndex.clear();
            m_dirtyFlags.clear();
            m_lastVisibleCount = 0;
            m_currentReadbackFrame = 0;
            // 初始化双缓冲帧
            for (auto& frame : m_readbackFrames)
            {
                frame.visibilityBuffer = RHI::NullHandle;
                frame.readbackBuffer = RHI::NullHandle;
                frame.fenceValue = 0;
                frame.ready = false;
            }
            m_readbackBuffer.clear();
            m_visiblePemIndices.clear();
        }

        void PersistentEntityManager::updateEntity(uint32_t index, const PersistentEntity& entity)
        {
            if (!m_initialized || index >= m_entityCount)
            {
                return;
            }

            m_entities[index] = entity;
            m_dirtyFlags[index] = true;
        }

        void PersistentEntityManager::uploadChanges()
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
            {
                return;
            }

            // 统计脏图元数量
            uint32_t dirtyCount = 0;
            for (uint32_t i = 0; i < m_entityCount; ++i)
            {
                if (m_dirtyFlags[i])
                {
                    ++dirtyCount;
                }
            }

            if (dirtyCount == 0)
            {
                return;
            }

            // 脏图元超过半数时走全量上传，避免多次小批量 upload 开销
            const bool fullUpload = (dirtyCount * 2 >= m_entityCount);

            if (fullUpload)
            {
                std::vector<EntityGpuData> gpuData;
                gpuData.reserve(m_entityCount);
                for (uint32_t i = 0; i < m_entityCount; ++i)
                {
                    const PersistentEntity& e = m_entities[i];
                    EntityGpuData gd{};
                    gd.bboxMin[0] = e.bboxMin[0];
                    gd.bboxMin[1] = e.bboxMin[1];
                    gd.bboxMin[2] = e.bboxMin[2];
                    gd.bboxMin[3] = 0.0f;
                    gd.bboxMax[0] = e.bboxMax[0];
                    gd.bboxMax[1] = e.bboxMax[1];
                    gd.bboxMax[2] = e.bboxMax[2];
                    gd.bboxMax[3] = 0.0f;
                    gd.worldPos[0] = e.worldPos[0];
                    gd.worldPos[1] = e.worldPos[1];
                    gd.worldPos[2] = e.worldPos[2];
                    gd.worldPos[3] = 0.0f;
                    gd.vertexOffset = e.vertexOffset;
                    gd.vertexCount = e.vertexCount;
                    gd.materialIndex = e.materialIndex;
                    gd.flags = e.flags;
                    gpuData.push_back(gd);
                }

                m_device->uploadBuffer(m_entityBuffer, 0, m_entityCount * sizeof(EntityGpuData), gpuData.data());

                // SY_DEBUGF("[PersistentEntityManager] full upload: %u entities", m_entityCount);
            }
            else
            {
                // 增量上传：只上传脏图元到其对应偏移位置
                for (uint32_t i = 0; i < m_entityCount; ++i)
                {
                    if (!m_dirtyFlags[i])
                    {
                        continue;
                    }

                    const PersistentEntity& e = m_entities[i];
                    EntityGpuData gd{};
                    gd.bboxMin[0] = e.bboxMin[0];
                    gd.bboxMin[1] = e.bboxMin[1];
                    gd.bboxMin[2] = e.bboxMin[2];
                    gd.bboxMin[3] = 0.0f;
                    gd.bboxMax[0] = e.bboxMax[0];
                    gd.bboxMax[1] = e.bboxMax[1];
                    gd.bboxMax[2] = e.bboxMax[2];
                    gd.bboxMax[3] = 0.0f;
                    gd.worldPos[0] = e.worldPos[0];
                    gd.worldPos[1] = e.worldPos[1];
                    gd.worldPos[2] = e.worldPos[2];
                    gd.worldPos[3] = 0.0f;
                    gd.vertexOffset = e.vertexOffset;
                    gd.vertexCount = e.vertexCount;
                    gd.materialIndex = e.materialIndex;
                    gd.flags = e.flags;

                    m_device->uploadBuffer(m_entityBuffer, i * sizeof(EntityGpuData), sizeof(EntityGpuData), &gd);
                }
            }

            // 重置所有脏标记
            for (uint32_t i = 0; i < m_entityCount; ++i)
            {
                m_dirtyFlags[i] = false;
            }
        }

        bool PersistentEntityManager::ensureCullingPipeline()
        {
            if (!m_device || m_cullingPipeline != RHI::NullHandle)
            {
                return true;
            }

            RHI::PipelineDesc desc{};
            desc.computeShader = "culling_comp";
            desc.vertexShader = nullptr;
            desc.fragmentShader = nullptr;
            desc.topology = RHI::PrimitiveTopology::PointList;  // 计算管线不依赖拓扑类型
            desc.vertexFormat = RHI::VertexFormat::P3C3;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = false;

            m_cullingPipeline = m_device->createPipeline(desc);

            if (m_cullingPipeline == RHI::NullHandle)
            {
                // macOS 仅支持 OpenGL 4.1（无 compute shader，需 4.3+），
                // GPU 剔除降级为 CPU 四叉树剔除，功能不受影响，不视为警告。
                SY_DEBUG("[PersistentEntityManager] GPU culling unavailable, falling back to CPU culling");
                return true;
            }
            else
            {
                SY_DEBUG("[PersistentEntityManager] culling pipeline created");
                return true;
            }
        }

        void PersistentEntityManager::executeCulling(float viewMinX, float viewMinY, float viewMaxX, float viewMaxY)
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
            {
                return;
            }

            if (m_cullingPipeline == RHI::NullHandle)
            {
                // GPU 剔除不可用（compute 不受支持），调用方将回退到 CPU 四叉树
                return;
            }

            // 清零原子计数器
            uint32_t zero = 0;
            m_device->uploadBuffer(m_countBuffer, 0, sizeof(uint32_t), &zero);

            // 绑定计算管线
            m_device->bindPipeline(m_cullingPipeline);

            // 绑定 SSBO
            m_device->bindShaderStorageBuffer(0, 0, m_entityBuffer, 0, m_entityCount * sizeof(EntityGpuData));
            m_device->bindShaderStorageBuffer(0, 1, m_visibilityBuffer, 0, m_entityCount * sizeof(uint32_t));
            m_device->bindShaderStorageBuffer(0, 2, m_indirectBuffer, 0, m_entityCount * sizeof(DrawIndirectCmd));
            m_device->bindShaderStorageBuffer(0, 3, m_countBuffer, 0, sizeof(uint32_t));

            // 上传 2D 视图矩形作为 uniform vec4
            float viewBounds[4] = { viewMinX, viewMinY, viewMaxX, viewMaxY };
            m_device->setUniformVec4("uViewBounds", viewBounds);
            m_device->setUniformInt("uEntityCount", static_cast<int>(m_entityCount));

            // 计算调度大小（每个线程处理一个图元）
            uint32_t groupsX = (m_entityCount + 255) / 256;
            m_device->dispatchCompute(groupsX, 1, 1);

            // 确保计算着色器写入完成后，后续绘制命令才能读取
            m_device->memoryBarrier(static_cast<uint32_t>(RHI::BarrierFlag::ShaderStorage | RHI::BarrierFlag::Command));
        }

uint32_t PersistentEntityManager::readBackGpuVisibility(uint32_t* outIndices, uint32_t maxCount)
{
    if (!m_initialized || !m_device || m_entityCount == 0)
    {
        return 0;
    }

    // 切换到下一帧的读回缓冲（双缓冲）
    m_currentReadbackFrame = (m_currentReadbackFrame + 1) % 2;
    auto& frame = m_readbackFrames[m_currentReadbackFrame];

    // 如果当前帧的缓冲还没有准备好（上一帧的读回还在进行中），则等待 fence
    if (frame.ready)
    {
        // 检查 fence 是否已就绪（非阻塞检查）
        bool fenceSignaled = m_device->checkFence(frame.fenceValue);
        if (!fenceSignaled)
        {
            // Fence 未就绪，返回上一帧的数据（或 0）
            // 实际生产环境中这里可能调用设备的 waitForFence
            return 0;
        }
        // Fence 已就绪，重置 ready 标记
        frame.ready = false;
    }

    // 使用当前帧的读回缓冲进行全量映射
    m_readbackBuffer.resize(m_entityCount);

    // 全量映射可见性缓冲：visibility buffer 是逐图元稀疏标记
    // （1=可见, 0=不可见），可见图元位置不连续，必须全量扫描
    // 注意：映射的是 executeCulling 写入的 m_visibilityBuffer，
    // 双缓冲帧的 visibilityBuffer 从未分配，映射会失败
    void* visMapped = m_device->mapBuffer(m_visibilityBuffer, 0, m_entityCount * sizeof(uint32_t), 0x0001);
    if (!visMapped)
    {
        SY_ERRORF("[PersistentEntityManager] readBackGpuVisibility: map visibility buffer failed (%u bytes)",
            m_entityCount * sizeof(uint32_t));
        return 0;
    }
    std::memcpy(m_readbackBuffer.data(), visMapped, m_entityCount * sizeof(uint32_t));
    m_device->unmapBuffer(m_visibilityBuffer);

    // 收集可见 PEM 索引并转换为 RenderWorld 稠密索引写入 outIndices
    uint32_t written = 0;
    m_visiblePemIndices.clear();
    for (uint32_t i = 0; i < m_entityCount; ++i)
    {
        if (!m_readbackBuffer[i])
        {
            continue;
        }

        m_visiblePemIndices.push_back(i);

        if (outIndices && written < maxCount)
        {
            outIndices[written++] = (i < m_pemToRenderWorldIndex.size()) ? m_pemToRenderWorldIndex[i] : i;
        }
        else if (written < maxCount)
        {
            ++written;
        }
    }

    // SY_DEBUGF("[PersistentEntityManager] readBackGpuVisibility: visible=%u / %u entities", written, m_entityCount);

return written;
}

        /**
         * @brief 提交读回任务（异步）
         *
         * 异步提交 GPU 可见性查询结果的读回请求，
         * CPU 无需阻塞即可继续后续工作。
         * 每帧调用一次，通过 fence 机制确保数据一致性。
         *
         * @param fenceValue 当前帧的 fence 值，用于 GPU 完成检查
         */
        void PersistentEntityManager::submitGpuReadback(uint64_t fenceValue)
        {
            if (!m_initialized || !m_device)
            {
                return;
            }

            // 更新当前帧的 fence 值
            m_readbackFrames[m_currentReadbackFrame].fenceValue = fenceValue;

            // 标记当前帧的缓冲准备就绪（实际使用中由 GPU 写入 fence 后由 CPU 标记）
            // 这里简化处理：由调用方（renderFrame）在 GPU 完成后设置 ready = true
            m_readbackFrames[m_currentReadbackFrame].ready = true;
        }

        // ================================================================
        // 间接命令生成（M8 简化版：基于回读可见性）
        // 注意：实际绘制路径（renderFrame -> BatchQueue）并不使用此输出。
        // 本函数保留用于未来 GPU 驱动管线的能力扩展。
        // 不执行 mapBuffer；复用 readBackGpuVisibility 中填充的 m_visiblePemIndices。
        // ================================================================

        void PersistentEntityManager::generateIndirectCommands(
            RHI::BufferHandle outIndirectBuffer, uint32_t* outCommandCount)
        {
            uint32_t count = 0;

            if (m_initialized && m_device && !m_visiblePemIndices.empty() && outIndirectBuffer != RHI::NullHandle)
            {
                // 为每个可见 PEM 索引写入 DrawIndirectCmd
                const uint32_t visibleCount = static_cast<uint32_t>(m_visiblePemIndices.size());
                std::vector<DrawIndirectCmd> cmds;
                cmds.reserve(visibleCount);

                for (uint32_t i = 0; i < visibleCount; ++i)
                {
                    uint32_t pemIdx = m_visiblePemIndices[i];
                    if (pemIdx >= m_entityCount)
                    {
                        continue;
                    }

                    const PersistentEntity& e = m_entities[pemIdx];
                    DrawIndirectCmd cmd;
                    cmd.vertexCount = e.vertexCount;
                    cmd.instanceCount = 1u;
                    cmd.firstVertex = e.vertexOffset;
                    cmd.baseInstance = 0u;
                    cmds.push_back(cmd);
                }

                if (!cmds.empty())
                {
                    m_device->uploadBuffer(outIndirectBuffer, 0, cmds.size() * sizeof(DrawIndirectCmd), cmds.data());
                    count = static_cast<uint32_t>(cmds.size());
                }
            }

            m_lastVisibleCount = count;

            if (outCommandCount)
            {
                *outCommandCount = count;
            }
        }
    }  // namespace core
}  // namespace Render