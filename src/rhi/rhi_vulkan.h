/**
 * @file rhi_vulkan.h
 * @brief Vulkan render device implementation declaration
 *
 * Implements the IDevice interface using Vulkan API for cross-platform
 * GPU-accelerated rendering. Provides full resource management, command
 * submission, and state setting capabilities.
 *
 * This implementation requires the Vulkan SDK and supports Windows/Linux platforms.
 */
#pragma once
#include "rhi_device.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <set>
#include <vector>

// Forward declare Vulkan types when SDK is not available
#ifdef _WIN32
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
#include <vulkan/vulkan.h>

namespace render::rhi
{

    /**
     * @brief Vulkan render device implementation
     *
     * Wraps Vulkan API to implement the IDevice interface.
     * Manages VkDevice, VkCommandPool, VkSwapchainKHR, and descriptor pools.
     */
    class VulkanDevice : public IDevice
    {
    public:
        // Constants
        static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

        VulkanDevice();
        ~VulkanDevice() override;

        // IDevice interface implementation
        bool initialize(void* nativeWindow, uint32_t width, uint32_t height) override;
        void shutdown() override;

        BufferHandle createBuffer(const BufferDesc&) override;
        void destroyBuffer(BufferHandle) override;
        TextureHandle createTexture(const TextureDesc&) override;
        void destroyTexture(TextureHandle) override;
        PipelineHandle createPipeline(const PipelineDesc&) override;
        void destroyPipeline(PipelineHandle) override;

        void uploadBuffer(BufferHandle, uint64_t offset, uint64_t size, const void* data) override;
        void uploadTexture(TextureHandle, uint32_t mip, const void* data, uint32_t rowPitch) override;
        void* mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t mapFlags) override;
        void unmapBuffer(BufferHandle) override;
        void flushMappedRange(BufferHandle, uint64_t offset, uint64_t size) override;

        void beginFrame() override;
        void endFrame() override;
        void present() override;

        void bindPipeline(PipelineHandle) override;
        void bindVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) override;
        void bindIndexBuffer(BufferHandle, uint64_t offset) override;
        void bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindShaderStorageBuffer(
            uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindTexture(uint32_t set, uint32_t binding, TextureHandle) override;
        void setViewport(const Viewport&) override;
        void setScissor(const Scissor&) override;
        void setLineWidth(float width) override;

        void setUniformMatrix3(const char* name, const float* data) override;
        void setUniformMatrix4(const char* name, const float* data) override;
        void setUniformFloat(const char* name, float value) override;
        void setUniformInt(const char* name, int32_t value) override;
        void setUniformVec2(const char* name, const float* data) override;
        void setUniformVec3(const char* name, const float* data) override;
        void setUniformVec4(const char* name, const float* data) override;

        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount,
            uint32_t instanceCount,
            uint32_t firstIndex,
            int32_t vertexOffset,
            uint32_t firstInstance) override;
        void drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawIndexedIndirect(
            BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;

        void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
        void memoryBarrier(uint32_t barrierFlags) override;

        void setClearColor(float r, float g, float b, float a) override;
        void clear(uint32_t flags) override;
        void enableDepthTest(bool enable) override;
        void enableBlend(bool enable) override;

        void resize(uint32_t width, uint32_t height) override;
        uint64_t getGPUMemoryUsage() const override;
        void* getNativeContext() override;

    private:
        // Vulkan object handles
        VkInstance m_instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_deviceProperties{};
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;
        int32_t m_graphicsQueueFamily = -1;
        int32_t m_presentQueueFamily = -1;

        // Swapchain and frame resources
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> m_swapchainImages;
        VkFormat m_swapchainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
        VkExtent2D m_swapchainExtent{};
        std::vector<VkImageView> m_swapchainImageViews;
        std::vector<VkFramebuffer> m_swapchainFramebuffers;
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        VkPipelineLayout m_defaultPipelineLayout = VK_NULL_HANDLE;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;

        // Command resources
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_commandBuffers[MAX_FRAMES_IN_FLIGHT]{};
        VkFence m_inFlightFences[MAX_FRAMES_IN_FLIGHT]{};
        uint32_t m_currentFrame = 0;
        uint32_t m_commandBufferIndex = 0;
        uint32_t m_currentImageIndex = 0;

        // Descriptor pool
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet m_descriptorSets[4]{};

        // State tracking
        VkPipeline m_boundPipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        bool m_depthTestEnabled = false;
        bool m_blendEnabled = false;

        // Shader stages (loaded from SPIR-V or GLSL)
        std::vector<VkPipelineShaderStageCreateInfo> m_shaderStages;

        // Debug layer
        bool m_debugLayer = false;

        // Internal resource structures
        struct BufferResource;
        struct TextureResource;
        struct PipelineResource;

        // Resource maps
        std::unordered_map<uint64_t, std::unique_ptr<BufferResource>> m_buffers;
        std::unordered_map<uint64_t, std::unique_ptr<TextureResource>> m_textures;
        std::unordered_map<uint64_t, std::unique_ptr<PipelineResource>> m_pipelines;

        // Handle counters
        uint64_t m_nextBufferId = 1;
        uint64_t m_nextTextureId = 1;
        uint64_t m_nextPipelineId = 1;
        uint32_t m_drawCallCount = 0;

        // State tracking
        bool m_initialized = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        float m_clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
        float m_lineWidth = 1.0f;
        std::vector<float> m_uniformCache;
        std::vector<float> m_pushConstants;

        // Internal helpers
        bool createSwapchain(uint32_t width, uint32_t height);
        bool createFrameResources();
        void cleanupSwapchain();

        struct SwapchainSupportDetails
        {
            VkSurfaceCapabilitiesKHR capabilities;
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);
        VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available);
        VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& available);
        VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);
    };

    /**
     * @brief 创建 Vulkan 渲染设备实例
     *
     * @return Vulkan 设备实例指针，失败返回 nullptr
     */
    IDevice* createVulkanDevice();

}  // namespace render::rhi
