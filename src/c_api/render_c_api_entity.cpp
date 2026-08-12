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

    RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        PrimitiveType type, uint16_t materialIdx)
    {
        if (!dev) return 0;
        dev->addEntity(id, vertices, vertexCount, type, materialIdx);
        return 1;
    }

    RENDER_API void renderModifyEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        uint16_t materialIdx)
    {
        if (!dev) return;
        dev->modifyEntity(id, vertices, vertexCount, materialIdx);
    }

    RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id)
    {
        if (!dev) return;
        dev->removeEntity(id);
    }

    RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible)
    {
        if (!dev) return;
        dev->setEntityVisibility(id, visible != 0);
    }

    RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize)
    {
        if (!dev || !packet) return;

        const uint8_t* ptr = static_cast<const uint8_t*>(packet);
        const uint8_t* end = ptr + packetSize;

        uint32_t updateCount;
        std::memcpy(&updateCount, ptr, 4);
        ptr += 8;

        for (uint32_t i = 0; i < updateCount && ptr < end; ++i)
        {
            EntityUpdate upd;
            std::memcpy(&upd, ptr, sizeof(EntityUpdate));
            ptr += sizeof(EntityUpdate);

            const VertexP3C3* verts = reinterpret_cast<const VertexP3C3*>(ptr);
            ptr += upd.vertexCount * sizeof(VertexP3C3);

            switch (upd.op)
            {
                case UpdateOp::Add:
                    dev->addEntity(upd.entityId, verts, upd.vertexCount,
                        static_cast<PrimitiveType>(upd.primitiveType), upd.materialIndex);
                    break;
                case UpdateOp::Modify:
                    dev->modifyEntity(upd.entityId, verts, upd.vertexCount, upd.materialIndex);
                    break;
                case UpdateOp::Remove:
                    dev->removeEntity(upd.entityId);
                    break;
            }
        }
    }

    // ==================== 3D 网格管理 ====================

    RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
        const float* positions, const float* normals,
        const uint32_t* indices,
        uint32_t vertexCount, uint32_t indexCount)
    {
        if (!dev) return INVALID_MESH_ID;
        return dev->meshManager.registerMesh(positions, normals, indices, vertexCount, indexCount);
    }

    RENDER_API void renderUnregisterMesh(RenderDevice* dev, MeshId mesh)
    {
        if (!dev) return;
        dev->meshManager.unregisterMesh(mesh);
    }

    RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
        const float modelMatrix[16], uint32_t materialIdx,
        const float color[4])
    {
        if (!dev) return UINT32_MAX;
        return dev->meshManager.addInstance(mesh, modelMatrix, materialIdx, color);
    }

    RENDER_API void renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
        const float modelMatrix[16])
    {
        if (!dev) return;
        dev->meshManager.modifyInstance(instanceId, modelMatrix);
    }

    RENDER_API void renderRemoveInstance(RenderDevice* dev, uint32_t instanceId)
    {
        if (!dev) return;
        dev->meshManager.removeInstance(instanceId);
    }

    // ==================== 材质管理 ====================

    RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc)
    {
        if (!dev) return 0;
        return dev->addMaterial(*desc);
    }

    RENDER_API void renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc)
    {
        if (!dev) return;
        dev->updateMaterial(idx, *desc);
    }

    // ==================== World3D 图元管理 ====================

    RENDER_API void renderAddEntity3D(RenderDevice* dev, EntityId id,
        const float* positions, const float* normals,
        uint32_t vertexCount, const uint32_t* indices,
        uint32_t indexCount, uint16_t materialIndex)
    {
        if (!dev) return;
        std::vector<VertexP3N3> verts(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            verts[i].px = positions[i * 3 + 0];
            verts[i].py = positions[i * 3 + 1];
            verts[i].pz = positions[i * 3 + 2];
            verts[i].nx = normals ? normals[i * 3 + 0] : 0.0f;
            verts[i].ny = normals ? normals[i * 3 + 1] : 0.0f;
            verts[i].nz = normals ? normals[i * 3 + 2] : 1.0f;
        }
        dev->world3D.addEntity(id, verts.data(), vertexCount, indices, indexCount, materialIndex);
    }

    RENDER_API void renderRemoveEntity3D(RenderDevice* dev, EntityId id)
    {
        if (!dev) return;
        dev->world3D.removeEntity(id);
    }

    RENDER_API void renderClearWorld3D(RenderDevice* dev)
    {
        if (!dev) return;
        dev->world3D.clear();
    }
} // extern "C"