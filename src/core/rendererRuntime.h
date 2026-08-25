#pragma once

#include "render/runtime_session.h"
#include "../rhi/rhiDevice.h"
#include "pipelineStateManager.h"

#include <unordered_map>
#include <vector>
#include <cstring>
#include <mutex>

namespace Render
{
    namespace RT
    {

        class Runtime
        {
        public:
            Runtime();
            ~Runtime();

            bool create(const RuntimeDesc* desc);
            void destroy();

            RHI::IDevice* device() { return m_device; }
            core::PipelineStateManager* psm() { return &m_psm; }
            bool ownsDevice() const { return m_ownedDevice; }

            BufferHandle createBuffer(const RTBufferDesc* desc);
            void destroyBuffer(BufferHandle buffer);
            void uploadBuffer(BufferHandle buffer, uint64_t offset, uint64_t size, const void* data);
            RHI::BufferHandle rhiBuffer(BufferHandle buffer) const;

            PipelineHandle createPipeline(const RTPipelineDesc* desc);
            RHI::PipelineHandle rhiPipeline(PipelineHandle handle) const;
            // 按命令自身的 topology/vertexFormat/space 解析（缓存）一个管线，
            // 使 RTDrawCommand.topology 真正生效（默认管线拓扑是固定的，不能覆盖每笔绘制）。
            PipelineHandle resolvePipeline(const RTDrawCommand& cmd);
            uint16_t defaultPipeline(DefaultPipeline kind);
            bool hasDefaultPipelines() const { return m_defaultsReady; }

            TextureHandle createTexture(const RTTextureDesc* desc);
            void destroyTexture(TextureHandle texture);
            void updateTexture(TextureHandle texture, const RTTextureDesc* desc);
            RHI::TextureHandle rhiTexture(TextureHandle texture) const;

            uint16_t addMaterial(const MaterialDesc* desc);
            void updateMaterial(uint16_t index, const MaterialDesc* desc);
            const MaterialDesc* material(uint16_t index) const;

            void frameBegin();
            RTTransientAlloc allocTransient(uint64_t size);
            void frameEnd();

            // 瞬态数据走 CPU 暂存缓冲 + 显式 uploadBuffer（不依赖 GL 持久映射，
            // 兼容 macOS GL 4.1 等不支持 ARB_buffer_storage 的平台）。
            const uint8_t* transientStagingData() const { return m_transientStaging.empty() ? nullptr : m_transientStaging.data(); }
            uint64_t transientCursor() const { return m_transientCursor; }
            uint64_t transientBufferId() const { return m_transientId; }

            uint64_t gpuMemoryBytes() const;

        private:
            RHI::IDevice* m_device = nullptr;
            core::PipelineStateManager m_psm;
            bool m_ownedDevice = false;

            std::unordered_map<uint64_t, RHI::BufferHandle> m_buffers;
            uint64_t m_nextBufferId = 1;

            std::vector<RHI::PipelineHandle> m_pipelines;
            uint16_t m_defaults[static_cast<int>(DefaultPipeline::Count)] = { 0 };

            std::unordered_map<uint64_t, RHI::TextureHandle> m_textures;
            uint64_t m_nextTextureId = 1;

            std::vector<MaterialDesc> m_materials;

            RHI::BufferHandle m_transientBuffer = RHI::NullHandle;
            uint64_t m_transientId = 0;
            uint64_t m_transientCapacity = 0;
            std::vector<uint8_t> m_transientStaging;
            uint64_t m_transientCursor = 0;
            bool m_transientActive = false;

            bool m_defaultsReady = false;

            void ensureDefaultPipelines();
        };

    }  // namespace RT
}  // namespace Render
