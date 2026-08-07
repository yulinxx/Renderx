/**
 * @file render_c_api_entity.cpp
 * @brief 图元管理、3D网格管理、材质管理
 *
 * 从 render_c_api.cpp 拆分而来，包含：
 * - World2D 图元 CRUD（添加/修改/删除/可见性/批量更新）
 * - 3D 网格注册/注销/实例管理
 * - 材质管理
 */
#include "render_c_api_internal.h"

using namespace render;

extern "C" {
    // ==================== World2D 图元管理 ====================

    /**
     * @brief 添加图元到渲染世界
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param type 图元类型
     * @param materialIdx 材质索引
     * @return 是否成功（1表示成功，0表示失败）
     */
    RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        PrimitiveType type, uint16_t materialIdx)
    {
        if (!dev) return 0;
        dev->world2D.addEntity(id, vertices, vertexCount, type, materialIdx);
        return 1;
    }

    /**
     * @brief 修改图元的顶点数据
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     * @param vertices 新的顶点数据
     * @param vertexCount 顶点数量
     * @param materialIdx 材质索引
     */
    RENDER_API void renderModifyEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        uint16_t materialIdx)
    {
        if (!dev) return;
        dev->world2D.modifyEntity(id, vertices, vertexCount, materialIdx);
    }

    /**
     * @brief 从渲染世界移除图元
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     */
    RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id)
    {
        if (!dev) return;
        dev->world2D.removeEntity(id);
    }

    /**
     * @brief 设置图元可见性
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     * @param visible 是否可见（非0表示可见）
     */
    RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible)
    {
        if (!dev) return;
        dev->world2D.setEntityVisibility(id, visible != 0);
    }

    /**
     * @brief 批量应用图元更新
     *
     * 从更新数据包中解析并应用一系列图元操作（添加、修改、删除）。
     *
     * @param dev 渲染设备指针
     * @param packet 更新数据包指针
     * @param packetSize 数据包大小
     */
    RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize)
    {
        if (!dev || !packet) return;

        const uint8_t* ptr = static_cast<const uint8_t*>(packet);
        const uint8_t* end = ptr + packetSize;

        // 解析更新数量（前4字节）
        uint32_t updateCount;
        std::memcpy(&updateCount, ptr, 4);
        ptr += 8; // 跳过8字节（4字节计数 + 4字节对齐）

        // 逐个解析并应用更新
        for (uint32_t i = 0; i < updateCount && ptr < end; ++i)
        {
            EntityUpdate upd;
            std::memcpy(&upd, ptr, sizeof(EntityUpdate));
            ptr += sizeof(EntityUpdate);

            // 获取顶点数据
            const VertexP3C3* verts = reinterpret_cast<const VertexP3C3*>(ptr);
            ptr += upd.vertexCount * sizeof(VertexP3C3);

            // 根据操作类型执行相应操作
            switch (upd.op)
            {
                case UpdateOp::Add:
                    dev->world2D.addEntity(upd.entityId, verts, upd.vertexCount,
                        static_cast<PrimitiveType>(upd.primitiveType), upd.materialIndex);
                    break;
                case UpdateOp::Modify:
                    dev->world2D.modifyEntity(upd.entityId, verts, upd.vertexCount, upd.materialIndex);
                    break;
                case UpdateOp::Remove:
                    dev->world2D.removeEntity(upd.entityId);
                    break;
            }
        }
    }

    // ==================== 3D 网格管理 ====================

    /**
     * @brief 注册3D网格
     *
     * @param dev 渲染设备指针
     * @param positions 顶点位置数组（每点3个float）
     * @param normals 顶点法线数组（每点3个float）
     * @param indices 索引数组
     * @param vertexCount 顶点数量
     * @param indexCount 索引数量
     * @return 网格ID
     */
    RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
        const float* positions, const float* normals,
        const uint32_t* indices,
        uint32_t vertexCount, uint32_t indexCount)
    {
        if (!dev) return INVALID_MESH_ID;
        return dev->meshManager.registerMesh(positions, normals, indices, vertexCount, indexCount);
    }

    /**
     * @brief 注销3D网格
     *
     * @param dev 渲染设备指针
     * @param mesh 网格ID
     */
    RENDER_API void renderUnregisterMesh(RenderDevice* dev, MeshId mesh)
    {
        if (!dev) return;
        dev->meshManager.unregisterMesh(mesh);
    }

    /**
     * @brief 添加网格实例
     *
     * @param dev 渲染设备指针
     * @param mesh 网格ID
     * @param modelMatrix 4x4模型矩阵
     * @param materialIdx 材质索引
     * @return 实例ID
     */
    RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
        const float modelMatrix[16], uint32_t materialIdx,
        const float color[4])
    {
        if (!dev) return UINT32_MAX;
        return dev->meshManager.addInstance(mesh, modelMatrix, materialIdx, color);
    }

    /**
     * @brief 修改网格实例的变换
     *
     * @param dev 渲染设备指针
     * @param instanceId 实例ID
     * @param modelMatrix 新的4x4模型矩阵
     */
    RENDER_API void renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
        const float modelMatrix[16])
    {
        if (!dev) return;
        dev->meshManager.modifyInstance(instanceId, modelMatrix);
    }

    /**
     * @brief 移除网格实例
     *
     * @param dev 渲染设备指针
     * @param instanceId 实例ID
     */
    RENDER_API void renderRemoveInstance(RenderDevice* dev, uint32_t instanceId)
    {
        if (!dev) return;
        dev->meshManager.removeInstance(instanceId);
    }

    // ==================== 材质管理 ====================

    /**
     * @brief 添加材质
     *
     * @param dev 渲染设备指针
     * @param desc 材质描述符
     * @return 材质索引
     */
    RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc)
    {
        if (!dev) return 0;
        return dev->world2D.addMaterial(desc);
    }

    /**
     * @brief 更新材质
     *
     * @param dev 渲染设备指针
     * @param idx 材质索引
     * @param desc 材质描述符
     */
    RENDER_API void renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc)
    {
        if (!dev) return;
        dev->world2D.updateMaterial(idx, desc);
    }
} // extern "C"