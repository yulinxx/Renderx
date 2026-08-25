#pragma once

#include "render/runtime_session.h"
#include "rendererRuntime.h"

#include <vector>
#include <cstring>

namespace Render
{
    namespace RT
    {

        class Session
        {
        public:
            Session();
            ~Session();

            bool create(const SessionDesc* desc);
            void destroy();

            void resize(uint32_t width, uint32_t height);
            void setClearColor(float r, float g, float b, float a);
            void setViewMatrix(const float viewMatrix[16]);

            void submitDrawCommands(const RTDrawPacket* packet);
            void queryVisibility(const float* aabbs, uint32_t aabbCount, const float viewBounds[4],
                RTVisibilityResult* out) const;
            void present();
            void getStats(RTStats* out) const;

        private:
            Runtime* m_rt = nullptr;
            float m_viewMatrix[16] = {};
            float m_viewport[4] = {};
            float m_clearColor[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
            uint32_t m_width = 1;
            uint32_t m_height = 1;
            void* m_nativeWindow = nullptr;

            uint32_t m_drawCalls = 0;
            uint32_t m_triangles = 0;
            uint32_t m_lines = 0;
            uint32_t m_points = 0;

            static uint32_t vertexStride(uint8_t fmt);
            static uint32_t indexStride(uint8_t type);

            void drawCommand(const RTDrawCommand& cmd);
        };

    }  // namespace RT
}  // namespace Render
