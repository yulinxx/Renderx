/**
 * @file persistent_entity_manager.cpp
 * @brief 持久图元管理器实现
 */
#include "persistent_entity_manager.h"
#include "Log/SyLogger.h"
#include <algorithm>
#include <cstring>

namespace render
{
    namespace core
    {
        bool PersistentEntityManager::initialize(rhi::IDevice* device, uint32_t maxEntities)
        {
            if (m_initialized || !device || maxEntities == 0)
                return false;

            m_device = device;
            m_maxEntities = maxEntities;
            m_entityCount = 0;

            m_entities.reserve(maxEntities);
            m_dirtyFlags.reserve(maxEntities);

            // ------------------------------------------------------------------------
            // 创建图元元数据 SSBO
            // ------------------------------------------------------------------------
            {
                rhi::BufferDesc desc;
                desc.size = maxEntities * sizeof(EntityGpuData);
                desc.usage = rhi::BufferUsage::ShaderStorage;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_SSBO";
                m_entityBuffer = device->createBuffer(desc);

                if (m_entityBuffer == rhi::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create entity buffer");
                    return false;
                }
            }

            // ------------------------------------------------------------------------
            // 创建可见性结果缓冲（每个图元一个 uint32：0=不可见 1=可见）
            // ------------------------------------------------------------------------
            {
                rhi::BufferDesc desc;
                desc.size = maxEntities * sizeof(uint32_t);
                desc.usage = rhi::BufferUsage::ShaderStorage;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Visibility";
                m_visibilityBuffer = device->createBuffer(desc);
                if (m_visibilityBuffer == rhi::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create visibility buffer");
                    return false;
                }

                // 初始化为全 0
                std::vector<uint32_t> zeros(maxEntities, 0);
                device->uploadBuffer(m_visibilityBuffer, 0,
                    maxEntities * sizeof(uint32_t), zeros.data());
            }

            // ------------------------------------------------------------------------
            // 创建 indirect draw 缓冲
            // ------------------------------------------------------------------------
            {
                rhi::BufferDesc desc;
                desc.size = maxEntities * sizeof(DrawIndirectCmd);
                desc.usage = rhi::BufferUsage::Indirect;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Indirect";
                m_indirectBuffer = device->createBuffer(desc);
                if (m_indirectBuffer == rhi::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create indirect buffer");
                    return false;
                }
            }

            // ------------------------------------------------------------------------
            // 创建原子计数缓冲（用于 compute shader 写入 indirect 命令数量）
            // ------------------------------------------------------------------------
            {
                rhi::BufferDesc desc;
                desc.size = sizeof(uint32_t);
                desc.usage = rhi::BufferUsage::ShaderStorage;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Count";
                m_countBuffer = device->createBuffer(desc);
                if (m_countBuffer == rhi::NullHandle)
                {
                    SY_ERROR("[PersistentEntityManager] failed to create count buffer");
                    return false;
                }

                uint32_t zero = 0;
                device->uploadBuffer(m_countBuffer, 0, sizeof(uint32_t), &zero);
            }

            if (!ensureCullingPipeline())
                return false;

            m_initialized = true;

            SY_DEBUGF("[PersistentEntityManager] initialized, maxEntities=%u", maxEntities);

            return true;
        }

        void PersistentEntityManager::shutdown()
        {
            if (!m_initialized || !m_device)
                return;

            if (m_entityBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_entityBuffer);
                m_entityBuffer = rhi::NullHandle;
            }

            if (m_visibilityBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_visibilityBuffer);
                m_visibilityBuffer = rhi::NullHandle;
            }

            if (m_indirectBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_indirectBuffer);
                m_indirectBuffer = rhi::NullHandle;
            }

            if (m_countBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_countBuffer);
                m_countBuffer = rhi::NullHandle;
            }

            if (m_cullingPipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_cullingPipeline);
                m_cullingPipeline = rhi::NullHandle;
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
                    m_pemToRenderWorldIndex[index] = renderWorldIndex;
                else
                    m_pemToRenderWorldIndex.push_back(renderWorldIndex);
            }
            else
            {
                // 兼容旧调用：以 entity.id 作为回退（不准确但能工作）
                if (index < m_pemToRenderWorldIndex.size())
                    m_pemToRenderWorldIndex[index] = entity.id;
                else
                    m_pemToRenderWorldIndex.push_back(entity.id);
            }

            //SY_DEBUGF("[PersistentEntityManager] addEntity id=%u at index=%u (rwIdx=%u)", entity.id, index, renderWorldIndex);

            return index;
        }

        void PersistentEntityManager::removeEntity(uint32_t index)
        {
            if (!m_initialized || index >= m_entityCount)
                return;

            // 惰性删除：标记为无效，vertexCount=0 表示该图元不参与绘制
            m_entities[index].vertexCount = 0;
            m_entities[index].flags = 0;
            m_dirtyFlags[index] = true;

            SY_DEBUGF("[PersistentEntityManager] removeEntity index=%u", index);
        }

        void PersistentEntityManager::clearEntities()
        {
            if (!m_initialized)
                return;

            m_entityCount = 0;
            m_entities.clear();
            m_pemToRenderWorldIndex.clear();
            m_dirtyFlags.clear();
            m_lastVisibleCount = 0;
            m_readbackBuffer.clear();
            m_visiblePemIndices.clear();
        }

        void PersistentEntityManager::updateEntity(uint32_t index, const PersistentEntity& entity)
        {
            if (!m_initialized || index >= m_entityCount)
                return;

            m_entities[index] = entity;
            m_dirtyFlags[index] = true;
        }

        void PersistentEntityManager::uploadChanges()
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
                return;

            // 统计脏图元数量
            uint32_t dirtyCount = 0;
            for (uint32_t i = 0; i < m_entityCount; ++i)
            {
                if (m_dirtyFlags[i])
                    ++dirtyCount;
            }

            if (dirtyCount == 0)
                return;

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
                    gd.bboxMin[0] = e.bboxMin[0]; gd.bboxMin[1] = e.bboxMin[1]; gd.bboxMin[2] = e.bboxMin[2]; gd.bboxMin[3] = 0.0f;
                    gd.bboxMax[0] = e.bboxMax[0]; gd.bboxMax[1] = e.bboxMax[1]; gd.bboxMax[2] = e.bboxMax[2]; gd.bboxMax[3] = 0.0f;
                    gd.worldPos[0] = e.worldPos[0]; gd.worldPos[1] = e.worldPos[1]; gd.worldPos[2] = e.worldPos[2]; gd.worldPos[3] = 0.0f;
                    gd.vertexOffset = e.vertexOffset;
                    gd.vertexCount = e.vertexCount;
                    gd.materialIndex = e.materialIndex;
                    gd.flags = e.flags;
                    gpuData.push_back(gd);
                }

                m_device->uploadBuffer(m_entityBuffer, 0,
                    m_entityCount * sizeof(EntityGpuData),
                    gpuData.data());

                SY_DEBUGF("[PersistentEntityManager] full upload: %u entities", m_entityCount);
            }
            else
            {
                // 增量上传：只上传脏图元到其对应偏移位置
                for (uint32_t i = 0; i < m_entityCount; ++i)
                {
                    if (!m_dirtyFlags[i])
                        continue;

                    const PersistentEntity& e = m_entities[i];
                    EntityGpuData gd{};
                    gd.bboxMin[0] = e.bboxMin[0]; gd.bboxMin[1] = e.bboxMin[1]; gd.bboxMin[2] = e.bboxMin[2]; gd.bboxMin[3] = 0.0f;
                    gd.bboxMax[0] = e.bboxMax[0]; gd.bboxMax[1] = e.bboxMax[1]; gd.bboxMax[2] = e.bboxMax[2]; gd.bboxMax[3] = 0.0f;
                    gd.worldPos[0] = e.worldPos[0]; gd.worldPos[1] = e.worldPos[1]; gd.worldPos[2] = e.worldPos[2]; gd.worldPos[3] = 0.0f;
                    gd.vertexOffset = e.vertexOffset;
                    gd.vertexCount = e.vertexCount;
                    gd.materialIndex = e.materialIndex;
                    gd.flags = e.flags;

                    m_device->uploadBuffer(m_entityBuffer,
                        i * sizeof(EntityGpuData),
                        sizeof(EntityGpuData), &gd);
                }

                SY_DEBUGF("[PersistentEntityManager] incremental upload: %u / %u entities",
                    dirtyCount, m_entityCount);
            }

            // 重置所有脏标记
            for (uint32_t i = 0; i < m_entityCount; ++i)
                m_dirtyFlags[i] = false;
        }

        bool PersistentEntityManager::ensureCullingPipeline()
        {
            if (!m_device || m_cullingPipeline != rhi::NullHandle)
                return true;

            rhi::PipelineDesc desc{};
            desc.computeShader = "culling_comp";
            desc.vertexShader = nullptr;
            desc.fragmentShader = nullptr;
            desc.topology = rhi::PrimitiveTopology::PointList; // 计算管线不依赖拓扑类型
            desc.vertexFormat = rhi::VertexFormat::P3C3;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = false;

            m_cullingPipeline = m_device->createPipeline(desc);

            if (m_cullingPipeline == rhi::NullHandle)
            {
                // 计算着色器不可用（如 macOS 仅支持 GL 4.1，无 compute shader）：
                // GPU 剔除降级为 CPU 四叉树剔除，初始化继续，不视为失败。
                SY_WARN("[PersistentEntityManager] GPU culling unavailable, falling back to CPU culling");
                return true;
            }
            else
            {
                SY_DEBUG("[PersistentEntityManager] culling pipeline created");
                return true;
            }
        }

        void PersistentEntityManager::executeCulling(float viewMinX, float viewMinY,
            float viewMaxX, float viewMaxY)
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
                return;

            if (m_cullingPipeline == rhi::NullHandle)
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
            m_device->bindShaderStorageBuffer(0, 0, m_entityBuffer, 0,
                m_entityCount * sizeof(EntityGpuData));
            m_device->bindShaderStorageBuffer(0, 1, m_visibilityBuffer, 0,
                m_entityCount * sizeof(uint32_t));
            m_device->bindShaderStorageBuffer(0, 2, m_indirectBuffer, 0,
                m_entityCount * sizeof(DrawIndirectCmd));
            m_device->bindShaderStorageBuffer(0, 3, m_countBuffer, 0, sizeof(uint32_t));

            // 上传 2D 视图矩形作为 uniform vec4
            float viewBounds[4] = { viewMinX, viewMinY, viewMaxX, viewMaxY };
            m_device->setUniformVec4("uViewBounds", viewBounds);
            m_device->setUniformInt("uEntityCount", static_cast<int>(m_entityCount));

            // 计算调度大小（每个线程处理一个图元）
            uint32_t groupsX = (m_entityCount + 255) / 256;
            m_device->dispatchCompute(groupsX, 1, 1);

            // 确保计算着色器写入完成后，后续绘制命令才能读取
            m_device->memoryBarrier(
                static_cast<uint32_t>(
                    rhi::BarrierFlag::ShaderStorage |
                    rhi::BarrierFlag::Command));
        }

        uint32_t PersistentEntityManager::readBackGpuVisibility(uint32_t* outIndices, uint32_t maxCount)
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
                return 0;

            // 复用成员缓冲，避免每帧临时分配（全量可见性缓冲 + 可见 PEM 索引缓存）
            m_readbackBuffer.resize(m_entityCount);

            // 全量映射可见性缓冲：visibility buffer 是逐图元稀疏标记
            //（1=可见, 0=不可见），可见图元位置不连续，必须全量扫描
            void* visMapped = m_device->mapBuffer(m_visibilityBuffer, 0,
                m_entityCount * sizeof(uint32_t), 0x0001);
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
                    continue;

                m_visiblePemIndices.push_back(i);

                if (outIndices && written < maxCount)
                {
                    outIndices[written++] = (i < m_pemToRenderWorldIndex.size())
                        ? m_pemToRenderWorldIndex[i] : i;
                }
                else if (written < maxCount)
                {
                    ++written;
                }
            }

            SY_DEBUGF("[PersistentEntityManager] readBackGpuVisibility: visible=%u / %u entities",
                written, m_entityCount);

            return written;
        }

        // ================================================================
        // 间接命令生成（M8 简化版：基于回读可见性）
        // 注意：实际绘制路径（renderFrame -> BatchQueue）并不使用此输出。
        // 本函数保留用于未来 GPU 驱动管线的能力扩展。
        // 不执行 mapBuffer；复用 readBackGpuVisibility 中填充的 m_visiblePemIndices。
        // ================================================================

        void PersistentEntityManager::generateIndirectCommands(
            rhi::BufferHandle outIndirectBuffer, uint32_t* outCommandCount)
        {
            uint32_t count = 0;

            if (m_initialized && m_device &&
                !m_visiblePemIndices.empty() && outIndirectBuffer != rhi::NullHandle)
            {
                // 为每个可见 PEM 索引写入 DrawIndirectCmd
                const uint32_t visibleCount = static_cast<uint32_t>(m_visiblePemIndices.size());
                std::vector<DrawIndirectCmd> cmds;
                cmds.reserve(visibleCount);

                for (uint32_t i = 0; i < visibleCount; ++i)
                {
                    uint32_t pemIdx = m_visiblePemIndices[i];
                    if (pemIdx >= m_entityCount)
                        continue;

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
                    m_device->uploadBuffer(outIndirectBuffer, 0,
                        cmds.size() * sizeof(DrawIndirectCmd), cmds.data());
                    count = static_cast<uint32_t>(cmds.size());
                }
            }

            m_lastVisibleCount = count;

            if (outCommandCount)
                *outCommandCount = count;

            SY_DEBUGF("[PersistentEntityManager] indirect commands: %u", count);
        }
    } // namespace core
} // namespace render