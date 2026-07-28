/**
 * @file persistent_entity_manager.cpp
 * @brief 持久图元管理器实现
 */
#include "persistent_entity_manager.h"
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        void PersistentEntityManager::initialize(rhi::IDevice* device, uint32_t maxEntities)
        {
            if (m_initialized || !device || maxEntities == 0)
                return;

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
            }

            // ------------------------------------------------------------------------
            // 创建可见性结果缓冲（每个图元一个 uint32，0=不可见, 1=可见）
            // ------------------------------------------------------------------------
            {
                rhi::BufferDesc desc;
                desc.size = maxEntities * sizeof(uint32_t);
                desc.usage = rhi::BufferUsage::ShaderStorage;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "PersistentEntity_Visibility";
                m_visibilityBuffer = device->createBuffer(desc);

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

                uint32_t zero = 0;
                device->uploadBuffer(m_countBuffer, 0, sizeof(uint32_t), &zero);
            }

            ensureCullingPipeline();

            m_initialized = true;
            SY_INFOF("[PersistentEntityManager] initialized, maxEntities=%u", maxEntities);
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

            SY_INFO("[PersistentEntityManager] shutdown");
        }

        uint32_t PersistentEntityManager::addEntity(const PersistentEntity& entity)
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

            SY_DEBUGF("[PersistentEntityManager] addEntity id=%u at index=%u", entity.id, index);
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
            m_dirtyFlags.clear();
            m_lastVisibleCount = 0;

            SY_DEBUG("[PersistentEntityManager] entities cleared");
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
            if (!m_initialized || !m_device)
                return;

            // 收集脏图元并上传
            std::vector<EntityGpuData> gpuData;
            gpuData.reserve(m_entityCount);

            uint32_t dirtyCount = 0;
            for (uint32_t i = 0; i < m_entityCount; ++i)
            {
                if (m_dirtyFlags[i])
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
                    m_dirtyFlags[i] = false;
                    ++dirtyCount;
                }
            }

            if (dirtyCount > 0)
            {
                // 简化处理：整批上传前 m_entityCount 个图元
                // 后续可优化为只上传 dirty 区间
                gpuData.clear();
                gpuData.reserve(m_entityCount);
                for (uint32_t i = 0; i < m_entityCount; ++i)
                {
                    const PersistentEntity& e = m_entities[i];
                    EntityGpuData gd;
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

                SY_DEBUGF("[PersistentEntityManager] uploaded %u entities", m_entityCount);
            }
        }

        void PersistentEntityManager::ensureCullingPipeline()
        {
            if (!m_device || m_cullingPipeline != rhi::NullHandle)
                return;

            rhi::PipelineDesc desc{};
            desc.computeShader = "culling_comp";
            desc.vertexShader = nullptr;
            desc.fragmentShader = nullptr;
            desc.topology = rhi::PrimitiveTopology::PointList; // compute pipeline 不依赖 topology
            desc.vertexFormat = rhi::VertexFormat::P3C3;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = false;

            m_cullingPipeline = m_device->createPipeline(desc);

            if (m_cullingPipeline == rhi::NullHandle)
            {
                SY_ERROR("[PersistentEntityManager] failed to create culling pipeline");
            }
            else
            {
                SY_INFO("[PersistentEntityManager] culling pipeline created");
            }
        }

        void PersistentEntityManager::executeCulling(float viewMinX, float viewMinY,
            float viewMaxX, float viewMaxY)
        {
            if (!m_initialized || !m_device || m_entityCount == 0)
                return;

            if (m_cullingPipeline == rhi::NullHandle)
            {
                SY_ERROR("[PersistentEntityManager] executeCulling: pipeline not ready");
                return;
            }

            // 清零原子计数器
            uint32_t zero = 0;
            m_device->uploadBuffer(m_countBuffer, 0, sizeof(uint32_t), &zero);

            // 绑定 compute pipeline
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

            // 计算 dispatch 大小（每个线程处理一个图元）
            uint32_t groupsX = (m_entityCount + 255) / 256;
            m_device->dispatchCompute(groupsX, 1, 1);

            // 确保 compute shader 写入完成后，后续绘制命令才能读取
            m_device->memoryBarrier(
                static_cast<uint32_t>(
                    rhi::BarrierFlag::ShaderStorage |
                    rhi::BarrierFlag::Command));

            SY_DEBUGF("[PersistentEntityManager] culling dispatched: %u entities, %u groups, view=[%.2f,%.2f,%.2f,%.2f]",
                m_entityCount, groupsX, viewMinX, viewMinY, viewMaxX, viewMaxY);
        }

        void PersistentEntityManager::generateIndirectCommands(rhi::BufferHandle outIndirectBuffer,
            uint32_t* outCommandCount)
        {
            (void)outIndirectBuffer; // 当前实现直接读取内部 countBuffer，参数预留用于未来扩展

            if (!m_initialized || !m_device)
            {
                if (outCommandCount) *outCommandCount = 0;
                return;
            }

            // 回读可见图元数量（原子计数器）
            uint32_t visibleCount = 0;
            // 注意：回读 GPU buffer 会阻塞 CPU，生产环境应使用异步查询或 GPU-driven 链路
            // 这里作为基础实现，直接读取
            // GL_MAP_READ_BIT = 0x0001
            void* mapped = m_device->mapBuffer(m_countBuffer, 0, sizeof(uint32_t), 0x0001);
            if (mapped)
            {
                visibleCount = *static_cast<uint32_t*>(mapped);
                m_device->unmapBuffer(m_countBuffer);
            }
            else
            {
                // 如果 map 失败，保守地回读全部可见性结果统计
                std::vector<uint32_t> vis(m_entityCount);
                void* visMapped = m_device->mapBuffer(m_visibilityBuffer, 0,
                    m_entityCount * sizeof(uint32_t), 0x0001);
                if (visMapped)
                {
                    std::memcpy(vis.data(), visMapped, m_entityCount * sizeof(uint32_t));
                    m_device->unmapBuffer(m_visibilityBuffer);
                    for (uint32_t v : vis)
                        if (v) ++visibleCount;
                }
            }

            m_lastVisibleCount = visibleCount;

            if (outCommandCount)
                *outCommandCount = visibleCount;

            SY_DEBUGF("[PersistentEntityManager] visible count: %u / %u",
                visibleCount, m_entityCount);
        }
    } // namespace core
} // namespace render