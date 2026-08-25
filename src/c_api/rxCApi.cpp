/**
 * @file rxCApi.cpp
 * @brief renderx.h 中全部 rx* 导出函数的实现
 *
 * 这一层只做三件事，不含任何渲染逻辑：
 * 1. 校验句柄与入参（旧实现在 C API 里对 Runtime/Session 句柄直接
 *    reinterpret_cast，非法句柄一律崩在解引用处）
 * 2. 把公共 POD 转成内部结构调用
 * 3. 把内部结果码翻成 RxResult，绝不让异常穿越 C ABI
 *
 * 句柄校验方式：Runtime/Session 句柄本质是指针，因此维护一份进程内的
 * 活跃对象登记表，解引用前先确认指针在表中。这不是并发契约——RenderX 的
 * 资源创建/销毁仍要求串行——互斥量只保证登记表本身不被撕裂。
 */

#include "rt/rxInternal.h"

#include <mutex>
#include <unordered_set>

namespace
{
    using namespace Render::RT;
    using namespace Render::RT::detail;

    /// 活跃对象登记表。仅用于句柄合法性校验，见文件头说明。
    struct HandleRegistry
    {
        std::mutex mutex;
        std::unordered_set<Runtime*> runtimes;
        std::unordered_set<Session*> sessions;

        static HandleRegistry& instance()
        {
            static HandleRegistry registry;
            return registry;
        }
    };

    void registerRuntime(Runtime* runtime)
    {
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        registry.runtimes.insert(runtime);
    }

    void unregisterRuntime(Runtime* runtime)
    {
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        registry.runtimes.erase(runtime);
    }

    void registerSession(Session* session)
    {
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        registry.sessions.insert(session);
    }

    void unregisterSession(Session* session)
    {
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        registry.sessions.erase(session);
    }

    /// 校验并解出 Runtime；非法句柄返回 nullptr 而不是崩溃
    Runtime* checkedRuntime(RuntimeHandle handle)
    {
        if (handle == RuntimeHandle::Invalid)
        {
            return nullptr;
        }
        Runtime* candidate = asRuntime(handle);
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        return registry.runtimes.count(candidate) != 0 ? candidate : nullptr;
    }

    Session* checkedSession(SessionHandle handle)
    {
        if (handle == SessionHandle::Invalid)
        {
            return nullptr;
        }
        Session* candidate = asSession(handle);
        HandleRegistry& registry = HandleRegistry::instance();
        std::lock_guard<std::mutex> guard(registry.mutex);
        return registry.sessions.count(candidate) != 0 ? candidate : nullptr;
    }
}  // namespace

namespace Render
{
    namespace RT
    {

        extern "C"
        {

        // ==================== 版本与静态查询 ====================

        uint32_t rxGetAbiVersion()
        {
            return RENDERX_ABI_VERSION;
        }

        const char* rxResultName(RxResult result)
        {
            switch (result)
            {
            case RxResult::Ok: return "Ok";
            // 正数不是失败：分配成功但底层缓冲已被替换，调用方需刷新句柄
            case RxResult::ErrorGeometryStoreGrown: return "GeometryStoreGrown";
            case RxResult::ErrorUnknown: return "ErrorUnknown";
            case RxResult::ErrorInvalidArgument: return "ErrorInvalidArgument";
            case RxResult::ErrorInvalidHandle: return "ErrorInvalidHandle";
            case RxResult::ErrorOutOfMemory: return "ErrorOutOfMemory";
            case RxResult::ErrorUnsupportedBackend: return "ErrorUnsupportedBackend";
            case RxResult::ErrorDeviceLost: return "ErrorDeviceLost";
            case RxResult::ErrorSurfaceLost: return "ErrorSurfaceLost";
            case RxResult::ErrorShaderCompilation: return "ErrorShaderCompilation";
            case RxResult::ErrorAbiVersionMismatch: return "ErrorAbiVersionMismatch";
            case RxResult::ErrorSurfaceOutOfDate: return "ErrorSurfaceOutOfDate";
            }
            return "Unknown";
        }

        const char* rxBackendName(Backend backend)
        {
            switch (backend)
            {
            case Backend::Null: return "Null";
            case Backend::OpenGL: return "OpenGL";
            case Backend::Metal: return "Metal";
            case Backend::Vulkan: return "Vulkan";
            case Backend::Auto: return "Auto";
            }
            return "Unknown";
        }

        uint8_t rxIsBackendAvailable(Backend backend)
        {
            if (backend == Backend::Auto)
            {
                // Auto 总能落到某个可用后端：preferredBackend 的兜底是 OpenGL，
                // 而 Null 后端在任何构建里都可用。
                return 1;
            }
            return RHI::isBackendAvailable(detail::toRhiBackend(backend)) ? 1 : 0;
        }

        // ==================== Runtime ====================

        RuntimeHandle rxRuntimeCreate(const RuntimeDesc* desc)
        {
            if (!desc)
            {
                return RuntimeHandle::Invalid;
            }
            if (desc->abiVersion != RENDERX_ABI_VERSION)
            {
                // 这里还不能用 desc 的日志回调之外的任何通道：Runtime 尚未建立。
                // ABI 不匹配意味着头文件与 DLL 版本不一致，继续下去就是静默错乱。
                if (desc->logCallback)
                {
                    char message[192];
                    std::snprintf(message, sizeof(message),
                                  "[rt] rxRuntimeCreate: ABI 版本不匹配（调用方 %u.%u，DLL %u.%u）",
                                  desc->abiVersion >> 16, desc->abiVersion & 0xFFFF,
                                  RENDERX_ABI_VERSION_MAJOR, RENDERX_ABI_VERSION_MINOR);
                    desc->logCallback(LogLevel::Error, message, desc->logUserData);
                }
                return RuntimeHandle::Invalid;
            }

            auto* runtime = new Runtime{};
            if (!runtime->create(*desc))
            {
                // create 内部失败时已自行回滚（含 destroy），这里只释放对象
                delete runtime;
                return RuntimeHandle::Invalid;
            }
            registerRuntime(runtime);
            return detail::toHandle(runtime);
        }

        void rxRuntimeDestroy(RuntimeHandle handle)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            // 先摘登记，再销毁：destroy 过程中若有回调重入，句柄已判为无效
            unregisterRuntime(runtime);

            // Runtime::destroy 会 delete 尚未销毁的 Session，这些 Session
            // 的句柄必须同时失效，否则宿主后续用旧句柄调用就会解引用野指针。
            {
                HandleRegistry& registry = HandleRegistry::instance();
                std::lock_guard<std::mutex> guard(registry.mutex);
                for (Session* session : runtime->sessions)
                {
                    registry.sessions.erase(session);
                }
            }

            runtime->destroy();
            delete runtime;
        }

        RxResult rxRuntimeGetCapabilities(RuntimeHandle handle, Capabilities* out)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!out)
            {
                return RxResult::ErrorInvalidArgument;
            }
            *out = runtime->caps;
            return RxResult::Ok;
        }

        // ==================== 缓冲 ====================

        BufferHandle rxBufferCreate(RuntimeHandle handle, const BufferDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return BufferHandle::Invalid;
            }
            return runtime->createBuffer(*desc);
        }

        void rxBufferDestroy(RuntimeHandle handle, BufferHandle buffer)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            runtime->destroyBuffer(buffer);
        }

        RxResult rxBufferUpload(RuntimeHandle handle, BufferHandle buffer, uint64_t offset,
                                uint64_t sizeBytes, const void* data)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return runtime->uploadBuffer(buffer, offset, sizeBytes, data);
        }

        // ==================== 管线 ====================

        uint16_t rxPipelineCreate(RuntimeHandle handle, const PipelineDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return 0;
            }
            return runtime->createPipeline(*desc);
        }

        uint16_t rxPipelineGetDefault(RuntimeHandle handle, DefaultPipeline kind)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return 0;
            }
            return runtime->defaultPipeline(kind);
        }

        // ==================== 纹理与材质 ====================

        TextureHandle rxTextureCreate(RuntimeHandle handle, const TextureDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return TextureHandle::Invalid;
            }
            return runtime->createTexture(*desc);
        }

        void rxTextureDestroy(RuntimeHandle handle, TextureHandle texture)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            runtime->destroyTexture(texture);
        }

        RxResult rxTextureUpdate(RuntimeHandle handle, TextureHandle texture, const TextureDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!desc)
            {
                return RxResult::ErrorInvalidArgument;
            }
            return runtime->updateTexture(texture, *desc);
        }

        uint16_t rxMaterialAdd(RuntimeHandle handle, const MaterialDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return 0;
            }
            return runtime->addMaterial(*desc);
        }

        RxResult rxMaterialUpdate(RuntimeHandle handle, uint16_t index, const MaterialDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!desc)
            {
                return RxResult::ErrorInvalidArgument;
            }
            return runtime->updateMaterial(index, *desc);
        }

        // ==================== 字体 ====================
        //
        // DLL 只出字形度量与图集，排版在调用方，理由见 rxFont.h 文件头。

        RxResult rxFontCreate(RuntimeHandle handle, const FontDesc* desc, FontHandle* outFont)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!desc || !outFont)
            {
                return RxResult::ErrorInvalidArgument;
            }
            return runtime->createFont(*desc, outFont);
        }

        void rxFontDestroy(RuntimeHandle handle, FontHandle font)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (runtime)
            {
                runtime->destroyFont(font);
            }
        }

        RxResult rxFontMetrics(RuntimeHandle handle, FontHandle font, FontMetrics* outMetrics)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return fontMetrics(*runtime, font, outMetrics);
        }

        RxResult rxFontGlyph(RuntimeHandle handle, FontHandle font, uint32_t codepoint,
                             GlyphInfo* outGlyph)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return fontGlyph(*runtime, font, codepoint, outGlyph);
        }

        RxResult rxFontFlushAtlas(RuntimeHandle handle, FontHandle font)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return fontFlushAtlas(*runtime, font);
        }

        TextureHandle rxFontAtlas(RuntimeHandle handle, FontHandle font)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return TextureHandle::Invalid;
            }
            return fontAtlas(*runtime, font);
        }

        // ==================== 持久几何仓 ====================
        //
        // 所有函数都要求传入 Runtime 句柄：仓与列表的所有权在 Runtime 上，
        // 只传仓句柄就无法校验它属于哪个 Runtime——跨 Runtime 误用是
        // 多窗口场景里最容易犯且最难查的错误。

        GeometryStoreHandle rxGeometryStoreCreate(RuntimeHandle handle, const GeometryStoreDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return GeometryStoreHandle::Invalid;
            }
            return runtime->createGeometryStore(*desc);
        }

        void rxGeometryStoreDestroy(RuntimeHandle handle, GeometryStoreHandle store)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            runtime->destroyGeometryStore(store);
        }

        BufferHandle rxGeometryStoreGetBuffer(RuntimeHandle handle, GeometryStoreHandle store)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return BufferHandle::Invalid;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            return target ? target->publicBuffer() : BufferHandle::Invalid;
        }

        RxResult rxGeometryAlloc(RuntimeHandle handle, GeometryStoreHandle store, uint64_t sizeBytes,
                                 GeometryBlock* out)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->allocate(sizeBytes, out);
        }

        RxResult rxGeometryWrite(RuntimeHandle handle, GeometryStoreHandle store, uint64_t blockId,
                                 uint32_t byteOffset, uint32_t sizeBytes, const void* data)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->write(blockId, byteOffset, sizeBytes, data);
        }

        RxResult rxGeometryFree(RuntimeHandle handle, GeometryStoreHandle store, uint64_t blockId)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->release(blockId);
        }

        RxResult rxGeometryFlush(RuntimeHandle handle, GeometryStoreHandle store)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->flush();
        }

        RxResult rxGeometryStoreGetStats(RuntimeHandle handle, GeometryStoreHandle store,
                                         GeometryStoreStats* out)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!out)
            {
                return RxResult::ErrorInvalidArgument;
            }
            GeometryStore* target = runtime->resolveGeometryStore(store);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            target->fillStats(out);
            return RxResult::Ok;
        }

        // ==================== 保留式绘制列表 ====================

        DrawListHandle rxDrawListCreate(RuntimeHandle handle, const DrawListDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return DrawListHandle::Invalid;
            }
            return runtime->createDrawList(*desc);
        }

        void rxDrawListDestroy(RuntimeHandle handle, DrawListHandle list)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            runtime->destroyDrawList(list);
        }

        RxResult rxDrawListUpsert(RuntimeHandle handle, DrawListHandle list, uint32_t slot,
                                  const DrawCommand* command, const float aabb[4])
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!command)
            {
                return RxResult::ErrorInvalidArgument;
            }
            DrawList* target = runtime->resolveDrawList(list);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->upsert(slot, *command, aabb);
        }

        RxResult rxDrawListRemove(RuntimeHandle handle, DrawListHandle list, uint32_t slot)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            DrawList* target = runtime->resolveDrawList(list);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->remove(slot);
        }

        RxResult rxDrawListClear(RuntimeHandle handle, DrawListHandle list)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            DrawList* target = runtime->resolveDrawList(list);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return target->clear();
        }

        RxResult rxDrawListGetStats(RuntimeHandle handle, DrawListHandle list, DrawListStats* out)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!out)
            {
                return RxResult::ErrorInvalidArgument;
            }
            DrawList* target = runtime->resolveDrawList(list);
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            target->fillStats(out);
            return RxResult::Ok;
        }

        // ==================== Surface ====================

        SurfaceHandle rxSurfaceCreate(RuntimeHandle handle, const SurfaceDesc* desc)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime || !desc)
            {
                return SurfaceHandle::Invalid;
            }
            return runtime->createSurface(*desc);
        }

        void rxSurfaceDestroy(RuntimeHandle handle, SurfaceHandle surface)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return;
            }
            runtime->destroySurface(surface);
        }

        RxResult rxSurfaceResize(RuntimeHandle handle, SurfaceHandle surface, uint32_t width,
                                 uint32_t height)
        {
            Runtime* runtime = checkedRuntime(handle);
            if (!runtime)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return runtime->resizeSurface(surface, width, height);
        }

        // ==================== Session ====================

        SessionHandle rxSessionCreate(const SessionDesc* desc)
        {
            if (!desc)
            {
                return SessionHandle::Invalid;
            }
            // Session 的 Runtime 句柄也要过校验：旧实现直接 cast，
            // 传一个已销毁的 Runtime 句柄就会在 create 内部解引用野指针。
            if (!checkedRuntime(desc->runtime))
            {
                return SessionHandle::Invalid;
            }

            auto* session = new Session{};
            if (!session->create(*desc))
            {
                delete session;
                return SessionHandle::Invalid;
            }
            registerSession(session);
            return detail::toHandle(session);
        }

        void rxSessionDestroy(SessionHandle handle)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return;
            }
            unregisterSession(session);
            session->destroy();
            delete session;
        }

        void rxSessionSetClearColor(SessionHandle handle, float r, float g, float b, float a)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return;
            }
            session->setClearColor(r, g, b, a);
        }

        void rxSessionSetViewMatrix(SessionHandle handle, const float viewMatrix[16])
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return;
            }
            session->setViewMatrix(viewMatrix);
        }

        RxResult rxSessionBeginFrame(SessionHandle handle)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->beginFrame();
        }

        RxResult rxSessionAllocTransient(SessionHandle handle, uint64_t sizeBytes, TransientAlloc* out)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->allocTransient(sizeBytes, out);
        }

        RxResult rxSessionSubmit(SessionHandle handle, const DrawPacket* packet)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!packet)
            {
                return RxResult::ErrorInvalidArgument;
            }
            return session->submit(*packet);
        }

        RxResult rxSessionSubmitDrawList(SessionHandle handle, DrawListHandle list,
                                         const float viewBounds[4])
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            // 列表通过 Session 自己的 Runtime 解析，因此无法用别的 Runtime
            // 的列表句柄画到这个窗口上——那会引用到不属于本设备的缓冲。
            DrawList* target = session->runtime ? session->runtime->resolveDrawList(list) : nullptr;
            if (!target)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->submitDrawList(target, viewBounds);
        }

        RxResult rxSessionEndFrame(SessionHandle handle)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->endFrame();
        }

        RxResult rxSessionReadPixels(SessionHandle handle, uint32_t x, uint32_t y, uint32_t width,
                                     uint32_t height, void* outBytes, uint64_t outByteCapacity)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->readPixels(static_cast<int32_t>(x), static_cast<int32_t>(y), width, height,
                                       outBytes, outByteCapacity);
        }

        RxResult rxSessionQueryVisibility(SessionHandle handle, const float* aabbs, uint32_t aabbCount,
                                          const float viewBounds[4], VisibilityResult* out)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            return session->queryVisibility(aabbs, aabbCount, viewBounds, out);
        }

        RxResult rxSessionGetStats(SessionHandle handle, FrameStats* out)
        {
            Session* session = checkedSession(handle);
            if (!session)
            {
                return RxResult::ErrorInvalidHandle;
            }
            if (!out)
            {
                return RxResult::ErrorInvalidArgument;
            }
            *out = session->stats;
            return RxResult::Ok;
        }

        // ==================== 工具 ====================

        uint64_t rxMakeSortKey(uint8_t layer, uint8_t transparent, uint16_t depth, uint16_t seq)
        {
            // layer(8) | transparent(8) | depth(16) | seq(16)，高位优先。
            // 高 16 位留空，便于将来扩展视口/渲染目标维度而不改变已有排序语义。
            return (static_cast<uint64_t>(layer) << 40) | (static_cast<uint64_t>(transparent) << 32) |
                   (static_cast<uint64_t>(depth) << 16) | static_cast<uint64_t>(seq);
        }

        }  // extern "C"

    }  // namespace RT
}  // namespace Render
