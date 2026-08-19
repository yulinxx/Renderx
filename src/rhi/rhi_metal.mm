/**
 * @file rhi_metal.mm
 * @brief Metal render device implementation — [D2-P1] EXPERIMENTAL / FROZEN
 *
 * [D2-P1 状态说明] 此 Metal 后端为结构残缺的实验性实现，当前不可渲染。
 * 已知问题：m_framebufferOnly 被 3 种不同类型指针复用（类型错乱）；
 * 无 .metal shader 文件；CAMetalLayer API 调用不存在。
 *
 * 决策：此文件已冻结（FROZEN），不再接受功能新增。
 * 如需 Metal 支持，请从零重建，而非修补此文件。
 * 非 GL 后端在 CMake 中默认不参与 Release 构建。
 *
 * Objective-C++ implementation of the Metal RHI backend.
 * Provides full IDevice interface implementation using Apple's Metal API.
 * Requires macOS 10.11+ or iOS 8+ and Metal framework.
 */

#include "rhi_metal.h"
#include "rhi_types.h"

#include <unordered_map>
#include <vector>
#include <cstring>
#include <cassert>

#include "Log/SyLogger.h"

// Metal framework imports
#ifdef __APPLE__
#import <Metal/Metal.hpp>
#import <QuartzCore/CAMetalLayer.hpp>
#endif

namespace render::rhi {

// ---------------------------------------------------------------------------
// Internal resource structures
// ---------------------------------------------------------------------------

struct MetalDevice::BufferResource {
    void* buffer = nullptr;  // MTLBuffer*
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    MemoryType memoryType = MemoryType::GPU_Only;
    bool isMapped = false;
    void* mappedPtr = nullptr;
    std::vector<uint8_t> stagingMemory;
};

struct MetalDevice::TextureResource {
    void* texture = nullptr;  // MTLTexture*
    void* sampler = nullptr;  // MTLSamplerState*
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::RGBA8;
    uint32_t mipLevels = 1;
};

struct MetalDevice::PipelineResource {
    void* pipelineState = nullptr;  // MTLRenderPipelineState*
    void* computePipelineState = nullptr;  // MTLComputePipelineState*
    void* depthStencilState = nullptr;  // MTLDepthStencilState*
    void* vertexFunction = nullptr;  // MTLFunction*
    void* fragmentFunction = nullptr;  // MTLFunction*
    void* computeFunction = nullptr;  // MTLFunction*
    void* pipelineLayout = nullptr;  // Not used in Metal, replaced by MTLArgumentEncoder
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    VertexFormat vertexFormat = VertexFormat::P3C3;
};

// ---------------------------------------------------------------------------
// Format mapping
// ---------------------------------------------------------------------------

static uint32_t toMetalFormat(Format fmt) {
#ifdef __APPLE__
    switch (fmt) {
        case Format::RGBA8:   return (uint32_t)MTL::PixelFormatRGBA8Unorm;
        case Format::RGBA32F: return (uint32_t)MTL::PixelFormatRGBA32Float;
        case Format::RG32F:   return (uint32_t)MTL::PixelFormatRG32Float;
        case Format::R32F:    return (uint32_t)MTL::PixelFormatR32Float;
        case Format::D32F:    return (uint32_t)MTL::PixelFormatDepth32Float;
        case Format::D24S8:   return (uint32_t)MTL::PixelFormatDepth24Unorm_Stencil8;
        case Format::R8:      return (uint32_t)MTL::PixelFormatR8Unorm;
        default:              return (uint32_t)MTL::PixelFormatInvalid;
    }
#else
    return 0;
#endif
}

static uint32_t toMetalPrimitiveTopology(PrimitiveTopology topo) {
#ifdef __APPLE__
    switch (topo) {
        case PrimitiveTopology::PointList:     return (uint32_t)MTL::PrimitiveTypePoint;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip:
        case PrimitiveTopology::LineLoop:      return (uint32_t)MTL::PrimitiveTypeLine;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip:
        case PrimitiveTopology::TriangleFan:   return (uint32_t)MTL::PrimitiveTypeTriangle;
        default:                               return (uint32_t)MTL::PrimitiveTypeTriangle;
    }
#else
    return 0;
#endif
}

static uint32_t toMetalIndexType(uint32_t) {
#ifdef __APPLE__
    return (uint32_t)MTL::IndexTypeUInt32;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// MetalDevice implementation
// ---------------------------------------------------------------------------

MetalDevice::MetalDevice() {
    // Initialize empty resource maps
}

MetalDevice::~MetalDevice() {
    // Destructor - shutdown should have been called
    if (m_initialized) {
        shutdown();
    }
}

bool MetalDevice::initialize(void* nativeWindow, uint32_t width, uint32_t height) {
    if (m_initialized) {
        SY_WARNF("MetalDevice::initialize: already initialized");
        return false;
    }

    m_width = width;
    m_height = height;

#ifdef __APPLE__
    // Create Metal device
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        SY_ERRORF("MetalDevice::initialize: failed to create Metal device");
        return false;
    }
    m_device = device;

    // Create command queue
    MTL::CommandQueue* commandQueue = device->newCommandQueue();
    if (!commandQueue) {
        SY_ERRORF("MetalDevice::initialize: failed to create command queue");
        return false;
    }
    m_commandQueue = commandQueue;

    // Create CAMetalLayer for the native window
    // nativeWindow is expected to be a NSView* or UIView* (platform view)
    CA::MetalLayer* metalLayer = CA::MetalLayer::layer();
    metalLayer->setDevice(device);
    metalLayer->setPixelFormats((MTL::PixelFormat)toMetalFormat(Format::RGBA8));
    metalLayer->setDrawableSize(CGSizeMake(width, height));
    metalLayer->setFramebufferOnly(true);

    // Attach layer to the native window view
    if (nativeWindow) {
        // On macOS, nativeWindow is NSView*
        // On iOS, nativeWindow is UIView*
        // layer add is platform-specific and handled by caller
    }
    m_framebufferOnly = metalLayer;

    // Create default library (shaders)
    MTL::Library* defaultLibrary = device->newDefaultLibrary();
    if (defaultLibrary) {
        m_defaultLibrary = defaultLibrary;
    }

    // Create render pass descriptor (created per-frame)
    m_renderPassDescriptor = nullptr;  // Created in beginFrame

    // Create depth buffer
    createDepthBuffer();

    m_initialized = true;
    SY_DEBUGF("MetalDevice::initialize: success (device: %s)",
              device->name()->utf8String());
    return true;
#else
    SY_ERRORF("MetalDevice::initialize: Metal not available on this platform");
    return false;
#endif
}

void MetalDevice::createDepthBuffer() {
#ifdef __APPLE__
    if (!m_device) return;

    MTL::Device* device = (MTL::Device*)m_device;
    MTL::TextureDescriptor* depthDesc = MTL::TextureDescriptor::alloc()->init();
    depthDesc->setTextureType(MTL::TextureType2D);
    depthDesc->setWidth(m_width);
    depthDesc->setHeight(m_height);
    depthDesc->setPixelFormat(MTL::PixelFormatDepth32Float);
    depthDesc->setMipmapped(false);
    depthDesc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);

    MTL::Texture* depthTexture = device->newTexture(depthDesc);
    m_framebufferOnly = depthTexture;  // Store depth texture reference
    depthDesc->release();
#endif
}

void MetalDevice::destroyDepthBuffer() {
#ifdef __APPLE__
    if (m_framebufferOnly) {
        MTL::Texture* depthTexture = (MTL::Texture*)m_framebufferOnly;
        if (depthTexture) {
            depthTexture->release();
        }
        m_framebufferOnly = nullptr;
    }
#endif
}

void MetalDevice::createRenderPass() {
    // Render pass is set up per-frame via MTLRenderPassDescriptor
    // This is a placeholder for future expansion
}

void MetalDevice::destroyRenderPass() {
    // Cleanup
}

bool MetalDevice::updateDrawable() {
#ifdef __APPLE__
    if (!m_framebufferOnly) return false;

    CA::MetalLayer* metalLayer = (CA::MetalLayer*)m_framebufferOnly;
    MTL::Drawable* drawable = metalLayer->nextDrawable();
    if (!drawable) return false;

    m_currentDrawable = drawable;

    MTL::RenderPassDescriptor* renderPassDesc = MTL::RenderPassDescriptor::alloc()->init();
    renderPassDesc->setColorAttachment(0)->setTexture(drawable->texture());
    renderPassDesc->getColorAttachment(0)->setLoadAction(MTL::LoadActionClear);
    MTL::ClearColor clearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    renderPassDesc->getColorAttachment(0)->setClearColor(clearColor);
    renderPassDesc->getColorAttachment(0)->setStoreAction(MTL::StoreActionStore);

    m_renderPassDescriptor = renderPassDesc;
    return true;
#else
    return false;
#endif
}

void MetalDevice::setupRenderEncoder() {
#ifdef __APPLE__
    if (!m_renderPassDescriptor || !m_device) return;

    MTL::Device* device = (MTL::Device*)m_device;
    MTL::CommandQueue* commandQueue = (MTL::CommandQueue*)m_commandQueue;

    MTL::CommandBuffer* commandBuffer = commandQueue->commandBuffer();
    if (!commandBuffer) return;

    MTL::RenderPassDescriptor* renderPassDesc = (MTL::RenderPassDescriptor*)m_renderPassDescriptor;
    MTL::RenderCommandEncoder* renderEncoder = commandBuffer->renderCommandEncoder(renderPassDesc);
    if (!renderEncoder) {
        SY_ERRORF("MetalDevice: failed to create render command encoder");
        return;
    }

    m_currentRenderEncoder = renderEncoder;

    // Set default viewport
    MTL::Viewport viewport(0, 0, m_width, m_height, 0.0f, 1.0f);
    renderEncoder->setViewport(viewport);

    // Store command buffer for endFrame
    m_framebufferOnly = commandBuffer;  // Store in a known location
#endif
}

void MetalDevice::endRenderEncoder() {
#ifdef __APPLE__
    if (m_currentRenderEncoder) {
        MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
        encoder->endEncoding();
        encoder->release();
        m_currentRenderEncoder = nullptr;
    }
#endif
}

void MetalDevice::shutdown() {
    if (!m_initialized) return;

#ifdef __APPLE__
    // Wait for GPU to finish
    if (m_device) {
        MTL::Device* device = (MTL::Device*)m_device;
        device->waitUntilIdle();
    }

    // Destroy pipelines
    for (auto& [handle, res] : m_pipelines) {
        if (res->pipelineState) {
            ((MTL::RenderPipelineState*)res->pipelineState)->release();
        }
        if (res->computePipelineState) {
            ((MTL::ComputePipelineState*)res->computePipelineState)->release();
        }
        if (res->depthStencilState) {
            ((MTL::DepthStencilState*)res->depthStencilState)->release();
        }
    }
    m_pipelines.clear();

    // Destroy textures
    for (auto& [handle, res] : m_textures) {
        if (res->texture) {
            ((MTL::Texture*)res->texture)->release();
        }
        if (res->sampler) {
            ((MTL::SamplerState*)res->sampler)->release();
        }
    }
    m_textures.clear();

    // Destroy buffers
    for (auto& [handle, res] : m_buffers) {
        if (res->buffer) {
            ((MTL::Buffer*)res->buffer)->release();
        }
    }
    m_buffers.clear();

    // Destroy framework objects
    if (m_defaultLibrary) {
        ((MTL::Library*)m_defaultLibrary)->release();
        m_defaultLibrary = nullptr;
    }

    if (m_renderPassDescriptor) {
        ((MTL::RenderPassDescriptor*)m_renderPassDescriptor)->release();
        m_renderPassDescriptor = nullptr;
    }

    destroyDepthBuffer();

    if (m_commandQueue) {
        ((MTL::CommandQueue*)m_commandQueue)->release();
        m_commandQueue = nullptr;
    }

    if (m_device) {
        ((MTL::Device*)m_device)->release();
        m_device = nullptr;
    }
#endif

    m_initialized = false;
    SY_DEBUGF("MetalDevice::shutdown: complete");
}

BufferHandle MetalDevice::createBuffer(const BufferDesc& desc) {
    auto res = std::make_unique<BufferResource>();
    res->size = desc.size;
    res->usage = desc.usage;
    res->memoryType = desc.memory;

#ifdef __APPLE__
    if (!m_device) return NullHandle;

    MTL::Device* device = (MTL::Device*)m_device;
    MTL::ResourceOptions options = MTL::ResourceStorageModeShared;

    if (desc.memory == MemoryType::GPU_Only) {
        options = MTL::ResourceStorageModePrivate;
    }

    MTL::BufferDescriptor* bufferDesc = MTL::BufferDescriptor::alloc()->init();
    bufferDesc->setLength(desc.size);
    bufferDesc->setResourceOptions(options);

    MTL::Buffer* buffer = device->newBuffer(bufferDesc);
    bufferDesc->release();

    if (!buffer) {
        SY_ERRORF("MetalDevice::createBuffer: failed");
        return NullHandle;
    }

    res->buffer = buffer;
#endif

    uint64_t typeTag = 1ULL << 60;  // Buffer type tag
    BufferHandle handle = makeHandle(typeTag, m_nextBufferId++);
    m_buffers[handle] = std::move(res);
    return handle;
}

void MetalDevice::destroyBuffer(BufferHandle handle) {
    auto it = m_buffers.find(handle);
    if (it != m_buffers.end()) {
#ifdef __APPLE__
        if (it->second->buffer) {
            ((MTL::Buffer*)it->second->buffer)->release();
        }
#endif
        m_buffers.erase(it);
    }
}

TextureHandle MetalDevice::createTexture(const TextureDesc& desc) {
    auto res = std::make_unique<TextureResource>();
    res->width = desc.width;
    res->height = desc.height;
    res->format = desc.format;
    res->mipLevels = desc.mipLevels;

#ifdef __APPLE__
    if (!m_device) return NullHandle;

    MTL::Device* device = (MTL::Device*)m_device;
    MTL::TextureDescriptor* texDesc = MTL::TextureDescriptor::alloc()->init();
    texDesc->setTextureType(MTL::TextureType2D);
    texDesc->setWidth(desc.width);
    texDesc->setHeight(desc.height);
    texDesc->setPixelFormat((MTL::PixelFormat)toMetalFormat(desc.format));
    texDesc->setMipmapped(desc.mipLevels > 1);
    texDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);

    MTL::Texture* texture = device->newTexture(texDesc);
    texDesc->release();

    if (!texture) {
        SY_ERRORF("MetalDevice::createTexture: failed");
        return NullHandle;
    }

    res->texture = texture;

    // Create sampler for color formats
    if (desc.format != Format::D32F && desc.format != Format::D24S8) {
        MTL::SamplerDescriptor* samplerDesc = MTL::SamplerDescriptor::alloc()->init();
        samplerDesc->setMinFilter(MTL::Magazine::Linear);
        samplerDesc->setMagFilter(MTL::Magazine::Linear);
        samplerDesc->setWrapR(MTL::SamplerAddressModeClampToEdge);
        samplerDesc->setWrapS(MTL::SamplerAddressModeClampToEdge);
        samplerDesc->setWrapT(MTL::SamplerAddressModeClampToEdge);

        MTL::SamplerState* sampler = device->newSamplerState(samplerDesc);
        samplerDesc->release();
        res->sampler = sampler;
    }
#endif

    uint64_t typeTag = 2ULL << 60;  // Texture type tag
    TextureHandle handle = makeHandle(typeTag, m_nextTextureId++);
    m_textures[handle] = std::move(res);
    return handle;
}

void MetalDevice::destroyTexture(TextureHandle handle) {
    auto it = m_textures.find(handle);
    if (it != m_textures.end()) {
#ifdef __APPLE__
        if (it->second->texture) {
            ((MTL::Texture*)it->second->texture)->release();
        }
        if (it->second->sampler) {
            ((MTL::SamplerState*)it->second->sampler)->release();
        }
#endif
        m_textures.erase(it);
    }
}

PipelineHandle MetalDevice::createPipeline(const PipelineDesc& desc) {
    auto res = std::make_unique<PipelineResource>();

#ifdef __APPLE__
    if (!m_device || !m_defaultLibrary) {
        SY_ERRORF("MetalDevice::createPipeline: device or library not ready");
        return NullHandle;
    }

    MTL::Device* device = (MTL::Device*)m_device;

    // Load vertex and fragment functions
    // Note: Assumes shader functions are named "vertex_main" and "fragment_main"
    MTL::Function* vertexFunc = nullptr;
    MTL::Function* fragmentFunc = nullptr;

    if (desc.vertexShader) {
        vertexFunc = (MTL::Function*)m_defaultLibrary->newFunction(desc.vertexShader);
    }
    if (!vertexFunc) {
        vertexFunc = (MTL::Function*)m_defaultLibrary->newFunction("vertex_main");
    }

    if (desc.fragmentShader) {
        fragmentFunc = (MTL::Function*)m_defaultLibrary->newFunction(desc.fragmentShader);
    }
    if (!fragmentFunc) {
        fragmentFunc = (MTL::Function*)m_defaultLibrary->newFunction("fragment_main");
    }

    if (!vertexFunc || !fragmentFunc) {
        SY_ERRORF("MetalDevice::createPipeline: failed to load shader functions");
        return NullHandle;
    }

    res->vertexFunction = vertexFunc;
    res->fragmentFunction = fragmentFunc;

    // Create render pipeline
    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    pipelineDesc->setVertexFunction(vertexFunc);
    pipelineDesc->setFragmentFunction(fragmentFunc);
    pipelineDesc->setPixelFormat(0, MTL::PixelFormatRGBA8Unorm);

    // Set vertex descriptor based on VertexFormat
    MTL::VertexDescriptor* vertexDesc = MTL::VertexDescriptor::alloc()->init();
    uint32_t stride = 0;

    switch (desc.vertexFormat) {
        case VertexFormat::P3C3: stride = 24; break;
        case VertexFormat::P3C4: stride = 28; break;
        case VertexFormat::P3N3: stride = 24; break;
        case VertexFormat::P3T2: stride = 20; break;
        case VertexFormat::P3T2C4: stride = 36; break;
        case VertexFormat::P2T2C4: stride = 32; break;
    }

    vertexDesc->setLayout(0, MTL::VertexAttributeDescriptor::alloc()->init());
    vertexDesc->layouts()->object(0)->setStride(stride);
    vertexDesc->layouts()->object(0)->setStepRate(1);
    vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexInputRatePerVertex);

    // Position attribute
    vertexDesc->attributes()->object(0)->setFormat(MTL::VertexFormatFloat3);
    vertexDesc->attributes()->object(0)->setOffset(0);
    vertexDesc->attributes()->object(0)->setBufferIndex(0);

    // Additional attributes based on format
    if (desc.vertexFormat == VertexFormat::P3C3 || desc.vertexFormat == VertexFormat::P3N3) {
        vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat3);
        vertexDesc->attributes()->object(1)->setOffset(12);
        vertexDesc->attributes()->object(1)->setBufferIndex(0);
    } else if (desc.vertexFormat == VertexFormat::P3C4 || desc.vertexFormat == VertexFormat::P3T2C4) {
        vertexDesc->attributes()->object(1)->setFormat(MTL::VertexFormatFloat4);
        vertexDesc->attributes()->object(1)->setOffset(desc.vertexFormat == VertexFormat::P3C4 ? 12 : 20);
        vertexDesc->attributes()->object(1)->setBufferIndex(0);
    }

    pipelineDesc->setVertexDescriptor(vertexDesc);
    vertexDesc->release();

    // Depth / stencil
    if (desc.depthTest) {
        MTL::DepthStencilDescriptor* depthDesc = MTL::DepthStencilDescriptor::alloc()->init();
        depthDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
        depthDesc->setDepthWriteEnabled(true);
        MTL::DepthStencilState* depthState = device->newDepthStencilState(depthDesc);
        depthDesc->release();

        if (depthState) {
            res->depthStencilState = depthState;
        }
    }

    // Create pipeline state
    MTL::RenderPipelineError* error = nullptr;
    MTL::RenderPipelineState* pipelineState = device->newRenderPipelineState(pipelineDesc, &error);
    pipelineDesc->release();

    if (!pipelineState) {
        SY_ERRORF("MetalDevice::createPipeline: failed to create pipeline state");
        return NullHandle;
    }

    res->pipelineState = pipelineState;
    res->topology = desc.topology;
    res->vertexFormat = desc.vertexFormat;
#endif

    uint64_t typeTag = 3ULL << 60;  // Pipeline type tag
    PipelineHandle handle = makeHandle(typeTag, m_nextPipelineId++);
    m_pipelines[handle] = std::move(res);
    return handle;
}

void MetalDevice::destroyPipeline(PipelineHandle handle) {
    auto it = m_pipelines.find(handle);
    if (it != m_pipelines.end()) {
#ifdef __APPLE__
        if (it->second->pipelineState) {
            ((MTL::RenderPipelineState*)it->second->pipelineState)->release();
        }
        if (it->second->computePipelineState) {
            ((MTL::ComputePipelineState*)it->second->computePipelineState)->release();
        }
        if (it->second->depthStencilState) {
            ((MTL::DepthStencilState*)it->second->depthStencilState)->release();
        }
        if (it->second->vertexFunction) {
            ((MTL::Function*)it->second->vertexFunction)->release();
        }
        if (it->second->fragmentFunction) {
            ((MTL::Function*)it->second->fragmentFunction)->release();
        }
#endif
        m_pipelines.erase(it);
    }
}

void MetalDevice::uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t size, const void* data) {
    if (!data || size == 0) return;

    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::uploadBuffer: invalid buffer handle");
        return;
    }

#ifdef __APPLE__
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        void* dest = buffer->contents();
        if (dest) {
            std::memcpy(static_cast<char*>(dest) + offset, data, size);
            return;
        }
    }

    // Fallback to staging
    if (it->second->stagingMemory.size() < offset + size) {
        it->second->stagingMemory.resize(offset + size);
    }
    std::memcpy(it->second->stagingMemory.data() + offset, data, size);
#endif
}

void MetalDevice::uploadTexture(TextureHandle handle, uint32_t mip, const void* data, uint32_t rowPitch) {
    if (!data) return;

    auto it = m_textures.find(handle);
    if (it == m_textures.end()) {
        SY_ERRORF("MetalDevice::uploadTexture: invalid texture handle");
        return;
    }

#ifdef __APPLE__
    MTL::Texture* texture = (MTL::Texture*)it->second->texture;
    if (!texture) return;

    MTL::Region region = MTL::Region::Make2D(0, 0,
        std::max(1u, it->second->width >> mip),
        std::max(1u, it->second->height >> mip));

    texture->replace(region, mip, 0, data, rowPitch);
#endif
}

void* MetalDevice::mapBuffer(BufferHandle handle, uint64_t offset, uint64_t size, uint32_t mapFlags) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::mapBuffer: invalid buffer handle");
        return nullptr;
    }

#ifdef __APPLE__
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        void* contents = buffer->contents();
        if (contents) {
            it->second->isMapped = true;
            it->second->mappedPtr = static_cast<char*>(contents) + offset;
            return it->second->mappedPtr;
        }
    }
#endif

    // Fallback: use staging memory
    if (it->second->stagingMemory.size() < offset + size) {
        it->second->stagingMemory.resize(offset + size);
    }
    it->second->isMapped = true;
    it->second->mappedPtr = it->second->stagingMemory.data() + offset;
    return it->second->mappedPtr;
}

void MetalDevice::unmapBuffer(BufferHandle handle) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) return;

    it->second->isMapped = false;
    it->second->mappedPtr = nullptr;
}

void MetalDevice::flushMappedRange(BufferHandle handle, uint64_t offset, uint64_t size) {
    // Metal uses shared memory by default, so no explicit flush needed
    // For private storage mode resources, would need staging buffer approach
}

void MetalDevice::beginFrame() {
#ifdef __APPLE__
    if (!updateDrawable()) {
        SY_WARNF("MetalDevice::beginFrame: failed to acquire next drawable");
        return;
    }
    setupRenderEncoder();
#endif
}

void MetalDevice::endFrame() {
#ifdef __APPLE__
    endRenderEncoder();

    if (m_framebufferOnly && m_currentDrawable) {
        MTL::CommandBuffer* commandBuffer = (MTL::CommandBuffer*)m_framebufferOnly;
        commandBuffer->present((MTL::Drawable*)m_currentDrawable);
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
        
        if (m_renderPassDescriptor) {
            ((MTL::RenderPassDescriptor*)m_renderPassDescriptor)->release();
            m_renderPassDescriptor = nullptr;
        }
    }
#endif
}

void MetalDevice::present() {
    // Presentation is handled in endFrame() via drawable present
}

void MetalDevice::bindPipeline(PipelineHandle handle) {
    auto it = m_pipelines.find(handle);
    if (it == m_pipelines.end()) {
        SY_ERRORF("MetalDevice::bindPipeline: invalid pipeline handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    encoder->setRenderPipelineState((MTL::RenderPipelineState*)it->second->pipelineState);

    if (it->second->depthStencilState) {
        encoder->setDepthStencilState((MTL::DepthStencilState*)it->second->depthStencilState);
    }
#endif
}

void MetalDevice::bindVertexBuffer(uint32_t slot, BufferHandle handle, uint64_t offset) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::bindVertexBuffer: invalid buffer handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        encoder->setVertexBuffer(buffer, offset, slot);
    }
#endif
}

void MetalDevice::bindIndexBuffer(BufferHandle handle, uint64_t offset) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::bindIndexBuffer: invalid buffer handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        encoder->setIndexBuffer(buffer, offset, 0);
    }
#endif
}

void MetalDevice::bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::bindUniformBuffer: invalid buffer handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        encoder->setVertexBuffer(buffer, offset, binding);
        encoder->setFragmentBuffer(buffer, offset, binding);
    }
#endif
}

void MetalDevice::bindShaderStorageBuffer(uint32_t set, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size) {
    auto it = m_buffers.find(handle);
    if (it == m_buffers.end()) {
        SY_ERRORF("MetalDevice::bindShaderStorageBuffer: invalid buffer handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Buffer* buffer = (MTL::Buffer*)it->second->buffer;
    if (buffer) {
        encoder->setVertexBuffer(buffer, offset, binding);
        encoder->setFragmentBuffer(buffer, offset, binding);
    }
#endif
}

void MetalDevice::bindTexture(uint32_t set, uint32_t binding, TextureHandle handle) {
    auto it = m_textures.find(handle);
    if (it == m_textures.end()) {
        SY_ERRORF("MetalDevice::bindTexture: invalid texture handle");
        return;
    }

#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Texture* texture = (MTL::Texture*)it->second->texture;
    MTL::SamplerState* sampler = (MTL::SamplerState*)it->second->sampler;
    if (texture) {
        encoder->setFragmentTexture(texture, binding);
    }
    if (sampler) {
        encoder->setFragmentSamplerState(sampler, binding);
    }
#endif
}

void MetalDevice::setViewport(const Viewport& vp) {
#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::Viewport viewport(vp.x, vp.y, vp.w, vp.h, 0.0f, 1.0f);
    encoder->setViewport(viewport);
#endif
}

void MetalDevice::setScissor(const Scissor& sc) {
#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    MTL::ScissorRect scissor(sc.x, sc.y, sc.w, sc.h);
    encoder->setScissorRect(scissor);
#endif
}

void MetalDevice::setLineWidth(float width) {
    m_lineWidth = width;
#ifdef __APPLE__
    if (m_currentRenderEncoder) {
        MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
        encoder->setLineWidth(width);
    }
#endif
}

void MetalDevice::setUniformMatrix3(const char* name, const float* data) {
    // Metal uses uniform buffers, not string-named uniforms
    // Store in cache for buffer-based uniform update
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    if (0 + 9 <= m_uniformCache.size()) {
        std::memcpy(m_uniformCache.data() + 0, data, 9 * sizeof(float));
    }
}

void MetalDevice::setUniformMatrix4(const char* name, const float* data) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    if (0 + 16 <= m_uniformCache.size()) {
        std::memcpy(m_uniformCache.data() + 0, data, 16 * sizeof(float));
    }
}

void MetalDevice::setUniformFloat(const char* name, float value) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    m_uniformCache[16] = value;
}

void MetalDevice::setUniformInt(const char* name, int32_t value) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    m_uniformCache[17] = static_cast<float>(value);
}

void MetalDevice::setUniformVec2(const char* name, const float* data) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    if (18 + 2 <= m_uniformCache.size()) {
        std::memcpy(m_uniformCache.data() + 18, data, 2 * sizeof(float));
    }
}

void MetalDevice::setUniformVec3(const char* name, const float* data) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    if (20 + 3 <= m_uniformCache.size()) {
        std::memcpy(m_uniformCache.data() + 20, data, 3 * sizeof(float));
    }
}

void MetalDevice::setUniformVec4(const char* name, const float* data) {
    if (m_uniformCache.empty()) {
        m_uniformCache.resize(256, 0.0f);
    }
    if (24 + 4 <= m_uniformCache.size()) {
        std::memcpy(m_uniformCache.data() + 24, data, 4 * sizeof(float));
    }
}

void MetalDevice::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    m_drawCallCount += instanceCount > 0 ? instanceCount : 1;
#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    encoder->drawPrimitives((MTL::PrimitiveType)toMetalPrimitiveTopology(PrimitiveTopology::TriangleList),
        vertexCount, instanceCount, firstVertex, firstInstance);
#endif
}

void MetalDevice::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    m_drawCallCount += instanceCount > 0 ? instanceCount : 1;
#ifdef __APPLE__
    if (!m_currentRenderEncoder) return;

    MTL::RenderCommandEncoder* encoder = (MTL::RenderCommandEncoder*)m_currentRenderEncoder;
    encoder->drawIndexedPrimitives(
        (MTL::PrimitiveType)toMetalPrimitiveTopology(PrimitiveTopology::TriangleList),
        indexCount,
        toMetalIndexType(0),
        (MTL::Buffer*)nullptr,  // Should use bound index buffer
        firstIndex,
        instanceCount,
        vertexOffset,
        firstInstance);
#endif
}

void MetalDevice::drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) {
    SY_WARNF("MetalDevice::drawIndirect: not fully implemented");
    m_drawCallCount += drawCount;
}

void MetalDevice::drawIndexedIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) {
    SY_WARNF("MetalDevice::drawIndexedIndirect: not fully implemented");
    m_drawCallCount += drawCount;
}

void MetalDevice::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) {
#ifdef __APPLE__
    if (!m_device || !m_commandQueue) return;

    MTL::Device* device = (MTL::Device*)m_device;
    MTL::CommandBuffer* cmdBuffer = ((MTL::CommandQueue*)m_commandQueue)->commandBuffer();
    if (!cmdBuffer) return;

    MTL::ComputeCommandEncoder* computeEncoder = cmdBuffer->computeCommandEncoder();
    if (!computeEncoder) {
        cmdBuffer->release();
        return;
    }

    // Would need compute pipeline set up
    computeEncoder->dispatchThreadgroups(
        MTL::Size(groupsX, groupsY, groupsZ),
        MTL::Size(1, 1, 1));

    computeEncoder->endEncoding();
    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();
    cmdBuffer->release();
#endif
}

void MetalDevice::memoryBarrier(uint32_t barrierFlags) {
    // Metal uses automatic memory management via command buffer dependencies
    // No explicit barrier needed in most cases
}

void MetalDevice::setClearColor(float r, float g, float b, float a) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void MetalDevice::clear(uint32_t flags) {
    // Clear is handled via render pass descriptor in beginFrame
}

void MetalDevice::enableDepthTest(bool enable) {
    m_depthTestEnabled = enable;
}

void MetalDevice::enableBlend(bool enable) {
    m_blendEnabled = enable;
}

void MetalDevice::resize(uint32_t width, uint32_t height) {
    if (!m_initialized) return;

    m_width = width;
    m_height = height;

#ifdef __APPLE__
    if (m_framebufferOnly) {
        CA::MetalLayer* metalLayer = (CA::MetalLayer*)m_framebufferOnly;
        metalLayer->setDrawableSize(CGSizeMake(width, height));
    }
#endif

    destroyDepthBuffer();
    createDepthBuffer();
}

uint64_t MetalDevice::getGPUMemoryUsage() const {
#ifdef __APPLE__
    if (!m_device) return 0;

    MTL::Device* device = (MTL::Device*)m_device;
    // Metal doesn't expose VRAM usage directly, estimate from resources
    uint64_t total = 0;
    for (const auto& [handle, res] : m_buffers) {
        total += res->size;
    }
    for (const auto& [handle, res] : m_textures) {
        total += static_cast<uint64_t>(res->width) * res->height * 4;
    }
    return total;
#else
    return 0;
#endif
}

 void* MetalDevice::getNativeContext() {
     return m_device;
 }

 // 离屏渲染目标：Metal 后端暂未实现，均返回无效 / 空操作
 RenderTargetHandle MetalDevice::createRenderTarget(const RenderTargetDesc&) { return NullRenderTarget; }
 void MetalDevice::destroyRenderTarget(RenderTargetHandle) {}
 void MetalDevice::bindRenderTarget(RenderTargetHandle) {}
 void MetalDevice::bindDefaultTarget() {}
 void MetalDevice::readRenderTarget(RenderTargetHandle, void*, uint32_t) {}


// Factory function
IDevice* createMetalDevice() {
    return new MetalDevice();
}

} // namespace render::rhi
