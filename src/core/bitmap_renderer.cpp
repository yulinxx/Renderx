/**
 * @file bitmap_renderer.cpp
 * @brief 位图渲染器实现
 *
 * 对标 TextAtlas 的"纹理四边形"模式，但按 entityId 管理多张独立位图：
 *  - 像素数据通过 RHI createTexture/uploadTexture 上传为 RGBA8 纹理
 *  - 四角世界坐标（TL/TR/BL/BR）构建两个三角形（UV 0~1）
 *  - 使用 bitmap.vert/bitmap.frag 绘制
 *
 * camera-relative：由顶点着色器统一减去相机中心，CPU 侧保留世界坐标，
 * 避免重复平移导致位图与视图移动不同步。
 */
#include "bitmap_renderer.h"
#include "shader/shaders.h"
#include <cstring>
#include <limits>
#include "Log/SyLogger.h"

namespace render::core
{
    size_t BitmapRenderer::findIndex(uint64_t entityId) const
    {
        for (size_t i = 0; i < m_bitmaps.size(); ++i)
        {
            if (m_bitmaps[i].entityId == entityId)
            {
                return i;
            }
        }
        return std::numeric_limits<size_t>::max();
    }

    bool BitmapRenderer::initialize(rhi::IDevice* device)
    {
        m_device = device;

        {
            rhi::BufferDesc desc{};
            desc.size = 6 * sizeof(BitmapVertex);  // 两个三角形
            desc.usage = rhi::BufferUsage::Vertex;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "BitmapVB";
            m_vertexBuffer = device->createBuffer(desc);
            if (m_vertexBuffer == rhi::NullHandle)
            {
                SY_ERROR("BitmapRenderer::initialize: vertex buffer creation failed");
                return false;
            }
        }

        {
            rhi::PipelineDesc desc{};
            desc.topology = rhi::PrimitiveTopology::TriangleList;
            desc.vertexShader = shader::BITMAP_VERT;
            desc.fragmentShader = shader::BITMAP_FRAG;
            desc.computeShader = nullptr;
            desc.vertexFormat = rhi::VertexFormat::P3T2;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = true;
            desc.srcBlend = rhi::BlendFactor::SrcAlpha;
            desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.depthFunc = rhi::CompareFunc::Always;
            m_pipeline = device->createPipeline(desc);
            if (m_pipeline == rhi::NullHandle)
            {
                SY_ERROR("BitmapRenderer::initialize: pipeline creation failed");
                return false;
            }
        }

        SY_DEBUGF("BitmapRenderer::initialize: vb=%u pipeline=%u", m_vertexBuffer, m_pipeline);
        return true;
    }

    void BitmapRenderer::shutdown()
    {
        if (m_device)
        {
            for (auto& entry : m_bitmaps)
            {
                if (entry.texture != rhi::NullHandle)
                {
                    m_device->destroyTexture(entry.texture);
                    entry.texture = rhi::NullHandle;
                }
            }
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = rhi::NullHandle;
            }
            if (m_pipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_pipeline);
                m_pipeline = rhi::NullHandle;
            }
        }

        m_bitmaps.clear();
        m_device = nullptr;
    }

    void BitmapRenderer::set(uint64_t entityId, const uint8_t* rgba, int32_t w, int32_t h, const float corners[8])
    {
        // entityId 允许为 0：作为"单图槽位"供旧 renderSetBitmap 兼容 API 使用，
        // 场景实体 ID 从 1 起，因此 0 永不与真实图元冲突。
        if (!m_device || !rgba || w <= 0 || h <= 0 || !corners)
        {
            remove(entityId);
            return;
        }

        size_t idx = findIndex(entityId);
        if (idx == std::numeric_limits<size_t>::max())
        {
            // 新增条目
            BitmapEntry entry;
            entry.entityId = entityId;
            m_bitmaps.push_back(std::move(entry));
            idx = m_bitmaps.size() - 1;
        }

        BitmapEntry& entry = m_bitmaps[idx];

        // 像素尺寸或纹理句柄变化时重建纹理
        if (w != entry.width || h != entry.height || entry.texture == rhi::NullHandle)
        {
            if (entry.texture != rhi::NullHandle)
            {
                m_device->destroyTexture(entry.texture);
                entry.texture = rhi::NullHandle;
            }

            rhi::TextureDesc desc{};
            desc.width = static_cast<uint32_t>(w);
            desc.height = static_cast<uint32_t>(h);
            desc.format = rhi::Format::RGBA8;
            desc.mipLevels = 1;
            desc.debugName = "BitmapTex";
            entry.texture = m_device->createTexture(desc);
            if (entry.texture == rhi::NullHandle)
            {
                SY_ERRORF("BitmapRenderer::set: texture creation failed for entity %llu",
                    static_cast<unsigned long long>(entityId));
                m_bitmaps.erase(m_bitmaps.begin() + static_cast<ptrdiff_t>(idx));
                return;
            }
        }

        // 上传像素数据（rowPitch = w*4 字节，无对齐填充）
        m_device->uploadTexture(entry.texture, 0, rgba, w * 4);

        std::memcpy(entry.corners, corners, sizeof(entry.corners));
        entry.width = w;
        entry.height = h;
    }

    void BitmapRenderer::remove(uint64_t entityId)
    {
        // entityId 为 0 表示"单图槽位"（兼容旧 API），同样允许移除
        size_t idx = findIndex(entityId);
        if (idx == std::numeric_limits<size_t>::max())
        {
            return;
        }

        BitmapEntry& entry = m_bitmaps[idx];
        if (m_device && entry.texture != rhi::NullHandle)
        {
            m_device->destroyTexture(entry.texture);
            entry.texture = rhi::NullHandle;
        }
        m_bitmaps.erase(m_bitmaps.begin() + static_cast<ptrdiff_t>(idx));
    }

    void BitmapRenderer::clear()
    {
        if (m_device)
        {
            for (auto& entry : m_bitmaps)
            {
                if (entry.texture != rhi::NullHandle)
                {
                    m_device->destroyTexture(entry.texture);
                    entry.texture = rhi::NullHandle;
                }
            }
        }
        m_bitmaps.clear();
    }

    void BitmapRenderer::render(rhi::IDevice* device, const float viewMatrix[9], const double cameraCenter[2])
    {
        if (!device || m_bitmaps.empty() || m_pipeline == rhi::NullHandle)
        {
            return;
        }

        device->bindPipeline(m_pipeline);
        // 与 CommandEncoder 的 World2D/Overlay 路径保持一致：camera-relative 渲染下
        // uViewMatrix 必须为 scale-only（去掉平移分量），平移统一由
        // shader 内的 relPos = aPosition - uCameraCenter 承担。
        // 若直接传完整 viewMatrix（含平移），再叠加 uCameraCenter 减法，
        // 会使平移量被加两倍，导致位图相对选择框整体偏移、缩放/平移时乱动。
        const float worldScaleMatrix[9] = { viewMatrix[0], 0.0f, 0.0f, 0.0f, viewMatrix[4], 0.0f, 0.0f, 0.0f, 1.0f };
        device->setUniformMatrix3("uViewMatrix", worldScaleMatrix);
        const float camCenter[2] = { static_cast<float>(cameraCenter[0]), static_cast<float>(cameraCenter[1]) };
        device->setUniformVec2("uCameraCenter", camCenter);
        device->bindVertexBuffer(0, m_vertexBuffer, 0);

        // 逐张位图绘制（位图数量通常较少，逐张 draw 简单且正确）
        for (const BitmapEntry& entry : m_bitmaps)
        {
            if (entry.texture == rhi::NullHandle || entry.width <= 0 || entry.height <= 0)
            {
                continue;
            }

            // 四角布局：TL, TR, BL, BR；UV 用于纹理采样
            struct Corner
            {
                float x, y;
                float u, v;
            };

            Corner corners[4] = {
                { entry.corners[0], entry.corners[1], 0.0f, 1.0f },  // TL: v=1（上）
                { entry.corners[2], entry.corners[3], 1.0f, 1.0f },  // TR
                { entry.corners[4], entry.corners[5], 0.0f, 0.0f },  // BL: v=0（下）
                { entry.corners[6], entry.corners[7], 1.0f, 0.0f },  // BR
            };

            // 两个三角形：TL-TR-BR, TL-BR-BL
            BitmapVertex verts[6];
            const int tri[6] = { 0, 1, 3, 0, 3, 2 };
            for (int i = 0; i < 6; ++i)
            {
                const Corner& c = corners[tri[i]];
                verts[i].px = c.x;
                verts[i].py = c.y;
                verts[i].pz = 0.0f;
                verts[i].u = c.u;
                verts[i].v = c.v;
            }

            device->uploadBuffer(m_vertexBuffer, 0, sizeof(verts), verts);
            device->bindTexture(0, 0, entry.texture);
            device->draw(6, 1, 0, 0);
        }
    }
}  // namespace render::core
