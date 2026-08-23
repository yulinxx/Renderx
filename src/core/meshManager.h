/**
 * @file meshManager.h
 * @brief 网格管理器类定义
 *
 * MeshManager 负责管理网格资源和实例渲染，支持：
 * - 网格注册/注销
 * - 实例化渲染（Instanced Rendering）
 * - 实例变换更新
 * - 可见性查询
 * - 多种渲染模式（图元、线框、高亮）
 *
 * 使用 SlotMap 管理网格，支持高效的插入和删除。
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include "../core/slotMap.h"
#include <vector>
#include <cstdint>

namespace Render::core
{

    /**
     * @brief 网格管理器类
     *
     * 管理网格数据和实例，负责上传和渲染。
     */
    class MeshManager
    {
    public:
        /// M6: 移除 MAX_INSTANCES 限制，改为动态分配 std::vector
        /// 旧代码: MAX_INSTANCES = 512（与 shader 数组大小一致）
        /// 新代码: 不再限制实例数量，动态调整缓冲区大小

    public:
        /**
         * @brief 初始化网格管理器
         *
         * @param device RHI设备指针
         * @return 初始化是否成功
         */
        bool initialize(RHI::IDevice* device);

        /**
         * @brief 关闭并释放所有资源
         */
        void shutdown();

        /**
         * @brief 注册一个新网格
         *
         * @param positions 顶点位置数组（每点3个float）
         * @param normals 顶点法线数组（每点3个float）
         * @param indices 索引数组
         * @param vertexCount 顶点数量
         * @param indexCount 索引数量
         * @return 网格ID
         */
        MeshId registerMesh(const float* positions,
            const float* normals,
            const uint32_t* indices,
            uint32_t vertexCount,
            uint32_t indexCount);

        /**
         * @brief 注销网格
         *
         * @param mesh 要注销的网格ID
         */
        void unregisterMesh(MeshId mesh);

        /**
         * @brief 添加网格实例
         *
         * @param mesh 网格ID
         * @param modelMatrix 4x4模型矩阵
         * @param materialIdx 材质索引
         * @return 实例ID
         */
        uint32_t addInstance(
            MeshId mesh, const float modelMatrix[16], uint32_t materialIdx, const float color[4] = nullptr);

        /**
         * @brief 修改实例变换
         *
         * @param instanceId 实例ID
         * @param modelMatrix 新的4x4模型矩阵
         */
        void modifyInstance(uint32_t instanceId, const float modelMatrix[16]);

        /**
         * @brief 移除实例
         *
         * @param instanceId 要移除的实例ID
         */
        void removeInstance(uint32_t instanceId);

        /**
         * @brief 设置实例可见性
         *
         * @param instanceId 实例ID
         * @param visible 是否可见
         */
        void setInstanceVisibility(uint32_t instanceId, bool visible);

        /**
         * @brief 查询可见实例
         *
         * @param viewMatrix 4x4视图矩阵
         * @param projMatrix 4x4投影矩阵
         * @param outInstanceIds 输出可见实例ID数组
         * @param outCount 输出可见实例数量
         * @param maxOut 最大输出数量
         */
        void queryVisible(const float viewMatrix[16],
            const float projMatrix[16],
            uint32_t* outInstanceIds,
            uint32_t* outCount,
            uint32_t maxOut);

        /**
         * @brief 更新内部状态（上传脏数据）
         */
        void update();

        /**
         * @brief 渲染所有可见网格实例
         *
         * @param device RHI设备指针
         * @param viewMatrix 4x4视图矩阵
         * @param projMatrix 4x4投影矩阵
         */
        void render(RHI::IDevice* device, const float viewMatrix[16], const float projMatrix[16]);

        /**
         * @brief 获取实例数量
         *
         * @return 实例数量
         */
        uint32_t getInstanceCount() const
        {
            return static_cast<uint32_t>(m_instances.size());
        }

    private:
        /**
         * @brief 网格条目结构
         *
         * 存储网格的顶点/索引偏移和数量。
         */
        struct MeshEntry
        {
            uint32_t indexOffset;   ///< 索引偏移
            uint32_t indexCount;    ///< 索引数量
            uint32_t vertexOffset;  ///< 顶点偏移
            uint32_t vertexCount;   ///< 顶点数量
            float bbox[6];          ///< 包围盒（minX, minY, minZ, maxX, maxY, maxZ）
            bool deleted;           ///< 是否已删除
        };

        /**
         * @brief 实例条目结构
         *
         * 存储实例的变换和材质信息。
         */
        struct InstanceEntry
        {
            float modelMatrix[16];   ///< 4x4模型矩阵
            float color[4];          ///< RGBA颜色
            uint32_t meshDenseIdx;   ///< 网格在稠密数组中的索引
            uint32_t materialIndex;  ///< 材质索引
            uint32_t flags;          ///< 实例标志（可见性等）
            bool dirty;              ///< 是否需要更新
        };

        /// 网格映射表
        SlotMap<uint64_t, MeshEntry> m_meshes;
        /// 顶点位置数据
        std::vector<VertexP3N3> m_positions;
        /// 索引数据
        std::vector<uint32_t> m_indices;

        /// 实例数组
        std::vector<InstanceEntry> m_instances;
        /// 脏实例索引数组（需要更新的实例）
        std::vector<uint32_t> m_dirtyInstances;
        /// 空闲实例索引数组
        std::vector<uint32_t> m_freeInstances;

        /// RHI设备指针
        RHI::IDevice* m_device = nullptr;
        /// 顶点位置缓冲区
        RHI::BufferHandle m_positionBuffer = RHI::NullHandle;
        /// 索引缓冲区
        RHI::BufferHandle m_indexBuffer = RHI::NullHandle;
        /// 实例缓冲区（存储实例变换矩阵）
        RHI::BufferHandle m_instanceBuffer = RHI::NullHandle;
        /// 图元渲染管线
        RHI::PipelineHandle m_meshPipeline = RHI::NullHandle;
        /// 线框渲染管线
        RHI::PipelineHandle m_wireframePipeline = RHI::NullHandle;
        /// 高亮渲染管线
        RHI::PipelineHandle m_highlightPipeline = RHI::NullHandle;
        /// 网格缓冲区是否需要上传
        bool m_meshBufferDirty = false;
        /// 实例缓冲区是否需要上传
        bool m_instanceBufferDirty = false;
        /// 实例缓冲区容量
        uint32_t m_instanceBufferCapacity = 0;

        /// 可见实例索引数组
        std::vector<uint32_t> m_visibleInstances;

        /**
         * @brief 构建渲染管线
         *
         * @param device RHI设备指针
         */
        void buildPipelines(RHI::IDevice* device);

        /**
         * @brief 上传网格缓冲区
         *
         * @param device RHI设备指针
         */
        void uploadMeshBuffers(RHI::IDevice* device);

        /**
         * @brief 上传实例缓冲区
         *
         * @param device RHI设备指针
         */
        void uploadInstanceBuffer(RHI::IDevice* device);
    };

}  // namespace Render::core
