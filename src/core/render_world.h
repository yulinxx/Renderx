/**
 * @file render_world.h
 * @brief 渲染世界类定义
 *
 * RenderWorld 是渲染系统的核心组件，负责管理场景中的所有图元、材质和顶点数据。
 * 主要功能包括：
 * - 图元的添加、修改、删除和可见性管理
 * - 基于四叉树的空间分区，实现高效的视锥体可见性查询
 * - 顶点缓冲区的分配和管理
 * - 材质管理
 *
 * 使用 SlotMap 实现稀疏索引到稠密索引的映射，支持 O(1) 的图元访问和删除。
 */
#pragma once

#include "render/render_types.h"
#include "../core/slot_map.h"
#include "../core/arena.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cmath>

namespace render
{
    namespace core
    {

        /**
         * @brief 渲染世界类
         *
         * 负责管理场景中的所有渲染图元，包括：
         * - 图元生命周期管理（添加、修改、删除）
         * - 顶点数据管理（动态分配和释放）
         * - 基于四叉树的空间分区和可见性查询
         * - 材质管理
         */
        class RenderWorld
        {
        private:
            /**
             * @brief 图元条目结构
             *
             * 存储单个图元的所有属性和状态信息。
             */
            struct EntityEntry
            {
                uint32_t vertexOffset;   ///< 在顶点池中的偏移量
                uint32_t vertexCount;    ///< 顶点数量
                uint32_t allocatedSize;  ///< 分配的顶点空间大小
                uint16_t primitiveType;  ///< 图元类型
                uint16_t materialIndex;  ///< 材质索引
                uint32_t flags;          ///< 图元标志位
                float bbox[4];           ///< 2D边界框 [minX, minY, maxX, maxY]
                bool dirty;              ///< 是否需要更新到GPU
                EntityId entityId;       ///< 图元唯一标识符
            };

            /**
             * @brief 四叉树节点结构
             *
             * 用于空间分区，实现高效的可见性查询。
             */
            struct QuadTreeNode
            {
                float minX, minY, maxX, maxY;  ///< 节点覆盖的区域
                uint32_t firstChild;           ///< 第一个子节点索引（非叶子节点）
                uint32_t firstEntity;          ///< 第一个图元索引（叶子节点）
                uint32_t entityCount;          ///< 节点内的图元数量
                bool isLeaf;                   ///< 是否为叶子节点
            };

            /**
             * @brief 材质条目结构
             */
            struct MaterialEntry
            {
                MaterialDesc desc;  ///< 材质描述
            };

            /// 四叉树每个节点最多包含的图元数量
            static constexpr uint32_t kQuadTreeMaxEntities = 64;
            /// 四叉树最大深度
            static constexpr uint32_t kQuadTreeMaxDepth = 8;
            /// 图元隐藏标志位
            static constexpr uint32_t kEntityFlagHidden = 1u << 0;

            /**
             * @brief 计算顶点的边界框
             *
             * @param verts 顶点数组
             * @param count 顶点数量
             * @param outMinX 输出最小X坐标
             * @param outMinY 输出最小Y坐标
             * @param outMaxX 输出最大X坐标
             * @param outMaxY 输出最大Y坐标
             */
            void computeBBox(const VertexP3C3* verts,
                uint32_t count,
                float* outMinX,
                float* outMinY,
                float* outMaxX,
                float* outMaxY) const;

            /**
             * @brief 重建四叉树
             *
             * 当图元数量变化超过阈值时调用，重新构建空间分区结构。
             */
            void rebuildQuadTree();

            /**
             * @brief 插入图元到四叉树
             *
             * @param nodeIdx 当前节点索引
             * @param entityDenseIdx 图元的稠密索引
             * @param depth 当前递归深度
             */
            void insertQuadTree(uint32_t nodeIdx, uint32_t entityDenseIdx, uint32_t depth);

            /**
             * @brief 检测两个边界框是否相交
             *
             * @param aMinX, aMinY, aMaxX, aMaxY 第一个边界框
             * @param bMinX, bMinY, bMaxX, bMaxY 第二个边界框
             * @return 是否相交
             */
            bool bboxIntersects(
                float aMinX, float aMinY, float aMaxX, float aMaxY, float bMinX, float bMinY, float bMaxX, float bMaxY)
                const;

            /**
             * @brief 检测图元是否在视锥体内可见
             *
             * @param denseIdx 图元的稠密索引
             * @param frustum 视锥体边界 [left, right, bottom, top]
             * @return 是否可见
             */
            bool isEntityVisible(uint32_t denseIdx, const float frustum[4]) const;

            /// 图元存储（稀疏索引到稠密索引的映射）
            SlotMap<uint64_t, EntityEntry> m_entities;
            /// 图元ID到稀疏索引的映射
            std::unordered_map<EntityId, uint64_t> m_entityKeyMap;
            /// 顶点池（所有图元的顶点数据存储在这里）
            std::vector<VertexP3C3> m_vertexPool;
            /// 空闲顶点空间链表
            std::vector<uint32_t> m_freeList;
            /// 需要更新的图元索引列表
            std::vector<uint32_t> m_dirtyList;
            /// 四叉树是否需要重建
            bool m_quadTreeDirty;
            /// 顶点池是否已调整大小
            bool m_vertexPoolResized;
            /// 图元变更计数器
            uint32_t m_changeCount;
            /// 上一帧的视图矩阵（用于检测视图变化）
            float m_lastViewMatrix[9];
            /// 四叉树节点数组
            std::vector<QuadTreeNode> m_quadTree;
            /// 四叉树图元索引数组
            std::vector<uint32_t> m_quadTreeEntities;
            /// 四叉树图元链表的下一个指针数组
            std::vector<uint32_t> m_quadTreeEntityNext;
            /// 材质列表
            std::vector<MaterialEntry> m_materials;
            /// 可见性查询结果缓存
            mutable std::vector<uint32_t> m_visibleResult;

            /// 触发四叉树重建的变更阈值
            static constexpr uint32_t kRebuildThreshold = 100;
            /// 视图矩阵变化检测的epsilon值
            static constexpr float kViewChangeEpsilon = 1e-6f;

            /// 世界重置代数计数器（每次 clearAllEntities 递增，用于检测顶点池重建）
            uint32_t m_generation = 0;

            /**
             * @brief 在顶点池中分配空间
             *
             * @param vertexCount 需要的顶点数量
             * @return 分配的偏移量（顶点数）
             */
            uint32_t allocateVertexSpace(uint32_t vertexCount);

            /**
             * @brief 释放顶点池中的空间
             *
             * @param offset 偏移量（顶点数）
             * @param size 大小（顶点数）
             */
            void deallocateVertexSpace(uint32_t offset, uint32_t size);

            /**
             * @brief 检测视图矩阵是否发生变化
             *
             * @param viewMatrix 当前视图矩阵
             * @return 是否变化
             */
            bool hasViewChanged(const float viewMatrix[9]) const;

        public:
            /**
             * @brief 初始化渲染世界
             *
             * @return 初始化是否成功
             */
            bool initialize();

            /**
             * @brief 关闭渲染世界并释放所有资源
             */
            void shutdown();

            /**
             * @brief 添加图元到场景
             *
             * @param id 图元唯一标识符
             * @param vertices 顶点数据
             * @param vertexCount 顶点数量
             * @param type 图元类型
             * @param materialIdx 材质索引
             */
            void addEntity(
                EntityId id, const VertexP3C3* vertices, uint32_t vertexCount, PrimitiveType type, uint16_t materialIdx);

            /**
             * @brief 修改已存在的图元
             *
             * @param id 图元唯一标识符
             * @param vertices 新的顶点数据
             * @param vertexCount 顶点数量
             * @param materialIdx 材质索引
             */
            void modifyEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount, uint16_t materialIdx);

            /**
             * @brief 从场景中移除图元
             *
             * @param id 图元唯一标识符
             */
            void removeEntity(EntityId id);

            /**
             * @brief 设置图元的可见性
             *
             * @param id 图元唯一标识符
             * @param visible 是否可见
             */
            void setEntityVisibility(EntityId id, bool visible);

            /**
             * @brief 清除所有图元
             */
            void clearAllEntities();

            /**
             * @brief 添加材质
             *
             * @param desc 材质描述
             * @return 材质索引
             */
            uint16_t addMaterial(const MaterialDesc* desc);

            /**
             * @brief 更新材质
             *
             * @param idx 材质索引
             * @param desc 新的材质描述
             */
            void updateMaterial(uint16_t idx, const MaterialDesc* desc);

            /**
             * @brief 查询可见图元
             *
             * 根据视图矩阵和视口尺寸，查询当前可见的图元。
             *
             * @param viewMatrix 3x3视图矩阵
             * @param viewWidth 视图宽度
             * @param viewHeight 视图高度
             * @param outIndices 输出可见图元的稠密索引数组
             * @param outCount 输出可见图元数量
             * @param maxOut 最大输出数量
             */
            void queryVisible(const float viewMatrix[9],
                float viewWidth,
                float viewHeight,
                uint32_t* outIndices,
                uint32_t* outCount,
                uint32_t maxOut) const;

            /**
             * @brief 顶点上传区间结构
             *
             * 描述顶点缓冲区中需要上传的连续区间。
             */
            struct VertexUploadRange
            {
                uint32_t vertexOffset;  ///< 顶点偏移（顶点数）
                uint32_t vertexCount;   ///< 顶点数量
            };

            /**
             * @brief 更新渲染世界状态
             *
             * 将脏图元的顶点数据上传到顶点池，并根据需要重建四叉树。
             */
            void update();

            /**
             * @brief 获取脏图元的顶点上传区间
             *
             * 返回自上次 clearDirtyFlags 以来发生变化的图元对应的顶点区间列表。
             * 用于 BatchQueue 做增量顶点上传。
             *
             * @param outRanges 输出区间数组（调用方提供缓冲）
             * @param maxRanges 最大输出区间数
             * @return 实际区间数量
             */
            uint32_t getDirtyVertexRanges(VertexUploadRange* outRanges, uint32_t maxRanges) const;

            /**
             * @brief 清除脏标志
             *
             * 在 BatchQueue 完成增量上传后调用，重置 dirty 标记。
             */
            void clearDirtyFlags();

            /**
             * @brief 获取顶点池数据指针
             *
             * @return 顶点数据指针
             */
            const VertexP3C3* getVertexData() const
            {
                return m_vertexPool.data();
            }

            /**
             * @brief 获取顶点池总顶点数
             *
             * @return 顶点数量
             */
            uint32_t getTotalVertexCount() const
            {
                return static_cast<uint32_t>(m_vertexPool.size());
            }

            /**
             * @brief 获取图元条目数组指针
             *
             * @return 图元条目指针
             */
            const EntityEntry* getEntityEntries() const;

            /**
             * @brief 获取图元数量
             *
             * @return 图元数量
             */
            uint32_t getEntityCount() const;

            /**
             * @brief 获取材质数组指针
             *
             * @return 材质数组指针
             */
            const MaterialEntry* getMaterials() const
            {
                return m_materials.data();
            }

            /**
             * @brief 获取材质数量
             *
             * @return 材质数量
             */
            uint16_t getMaterialCount() const
            {
                return static_cast<uint16_t>(m_materials.size());
            }

            /**
             * @brief 获取可见图元数量
             *
             * @return 可见图元数量
             */
            uint32_t getVisibleCount() const
            {
                return static_cast<uint32_t>(m_visibleResult.size());
            }

            /**
             * @brief 获取可见图元索引
             *
             * @param outIndices 输出索引数组指针
             * @param outCount 输出索引数量
             */
            void getVisibleIndices(const uint32_t** outIndices, uint32_t* outCount) const
            {
                *outIndices = m_visibleResult.data();
                *outCount = static_cast<uint32_t>(m_visibleResult.size());
            }

            /**
             * @brief 获取世界重置代数（用于检测顶点池是否被重建）
             *
             * @return 当前代数
             */
            uint32_t getGeneration() const
            {
                return m_generation;
            }
        };

        inline bool RenderWorld::bboxIntersects(
            float aMinX, float aMinY, float aMaxX, float aMaxY, float bMinX, float bMinY, float bMaxX, float bMaxY) const
        {
            return aMinX <= bMaxX && aMaxX >= bMinX && aMinY <= bMaxY && aMaxY >= bMinY;
        }

    }  // namespace core
}  // namespace render
