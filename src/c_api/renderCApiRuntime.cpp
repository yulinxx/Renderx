#include "render/runtime_session.h"
#include "core/rendererRuntime.h"
#include "core/rendererSession.h"

#include <cstdint>
#include <new>

namespace Render
{
    namespace RT
    {

        static Runtime* asRT(RuntimeHandle h) { return reinterpret_cast<Runtime*>(static_cast<uintptr_t>(h)); }
        static Session* asSession(SessionHandle h) { return reinterpret_cast<Session*>(static_cast<uintptr_t>(h)); }

        extern "C"
        {

            RENDER_API RuntimeHandle runtimeCreate(const RuntimeDesc* desc)
            {
                auto* rt = new (std::nothrow) Runtime();
                if (!rt)
                    return 0;
                if (!rt->create(desc))
                {
                    delete rt;
                    return 0;
                }
                return static_cast<RuntimeHandle>(reinterpret_cast<uintptr_t>(rt));
            }

            RENDER_API void runtimeDestroy(RuntimeHandle runtime)
            {
                delete asRT(runtime);
            }

            RENDER_API BufferHandle runtimeCreateBuffer(RuntimeHandle runtime, const RTBufferDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                return rt ? rt->createBuffer(desc) : 0;
            }

            RENDER_API void runtimeDestroyBuffer(RuntimeHandle runtime, BufferHandle buffer)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->destroyBuffer(buffer);
            }

            RENDER_API void runtimeUploadBuffer(RuntimeHandle runtime, BufferHandle buffer,
                uint64_t offset, uint64_t size, const void* data)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->uploadBuffer(buffer, offset, size, data);
            }

            RENDER_API PipelineHandle runtimeCreatePipeline(RuntimeHandle runtime, const RTPipelineDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                return rt ? rt->createPipeline(desc) : 0;
            }

            RENDER_API uint16_t runtimeGetDefaultPipeline(RuntimeHandle runtime, DefaultPipeline kind)
            {
                Runtime* rt = asRT(runtime);
                return rt ? rt->defaultPipeline(kind) : 0;
            }

            RENDER_API TextureHandle runtimeCreateTexture(RuntimeHandle runtime, const RTTextureDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                return rt ? rt->createTexture(desc) : 0;
            }

            RENDER_API void runtimeDestroyTexture(RuntimeHandle runtime, TextureHandle texture)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->destroyTexture(texture);
            }

            RENDER_API void runtimeUpdateTexture(RuntimeHandle runtime, TextureHandle texture, const RTTextureDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->updateTexture(texture, desc);
            }

            RENDER_API uint16_t runtimeAddMaterial(RuntimeHandle runtime, const MaterialDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                return rt ? rt->addMaterial(desc) : 0;
            }

            RENDER_API void runtimeUpdateMaterial(RuntimeHandle runtime, uint16_t index, const MaterialDesc* desc)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->updateMaterial(index, desc);
            }

            RENDER_API void runtimeFrameBegin(RuntimeHandle runtime)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->frameBegin();
            }

            RENDER_API void runtimeAllocTransient(RuntimeHandle runtime, uint64_t size, RTTransientAlloc* out)
            {
                Runtime* rt = asRT(runtime);
                if (out)
                    *out = rt ? rt->allocTransient(size) : RTTransientAlloc{};
            }

            RENDER_API void runtimeFrameEnd(RuntimeHandle runtime)
            {
                Runtime* rt = asRT(runtime);
                if (rt)
                    rt->frameEnd();
            }

            RENDER_API uint64_t rtMakeSortKey(uint8_t layer, uint8_t transparent, uint16_t depth, uint16_t seq)
            {
                uint64_t k = 0;
                k |= (static_cast<uint64_t>(layer) & 0xFF) << 56;
                k |= (static_cast<uint64_t>(transparent ? 1 : 0) & 0xFF) << 48;
                k |= (static_cast<uint64_t>(depth) & 0xFFFF) << 32;
                k |= (static_cast<uint64_t>(seq) & 0xFFFF);
                return k;
            }

            RENDER_API SessionHandle sessionCreate(const SessionDesc* desc)
            {
                auto* s = new (std::nothrow) Session();
                if (!s)
                    return 0;
                if (!s->create(desc))
                {
                    delete s;
                    return 0;
                }
                return static_cast<SessionHandle>(reinterpret_cast<uintptr_t>(s));
            }

            RENDER_API void sessionDestroy(SessionHandle session)
            {
                delete asSession(session);
            }

            RENDER_API void sessionResize(SessionHandle session, uint32_t width, uint32_t height)
            {
                Session* s = asSession(session);
                if (s)
                    s->resize(width, height);
            }

            RENDER_API void sessionSetClearColor(SessionHandle session, float r, float g, float b, float a)
            {
                Session* s = asSession(session);
                if (s)
                    s->setClearColor(r, g, b, a);
            }

            RENDER_API void sessionSetViewMatrix(SessionHandle session, const float viewMatrix[16])
            {
                Session* s = asSession(session);
                if (s)
                    s->setViewMatrix(viewMatrix);
            }

            RENDER_API void sessionSubmitDrawCommands(SessionHandle session, const RTDrawPacket* packet)
            {
                Session* s = asSession(session);
                if (s)
                    s->submitDrawCommands(packet);
            }

            RENDER_API void sessionQueryVisibility(SessionHandle session,
                const float* aabbs, uint32_t aabbCount, const float viewBounds[4], RTVisibilityResult* out)
            {
                Session* s = asSession(session);
                if (s)
                    s->queryVisibility(aabbs, aabbCount, viewBounds, out);
            }

            RENDER_API void sessionPresent(SessionHandle session)
            {
                Session* s = asSession(session);
                if (s)
                    s->present();
            }

            RENDER_API void sessionGetStats(SessionHandle session, RTStats* out)
            {
                Session* s = asSession(session);
                if (s)
                    s->getStats(out);
            }

        }  // extern "C"

    }  // namespace RT
}  // namespace Render
