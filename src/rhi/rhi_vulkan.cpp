/**
 * @file rhi_vulkan.cpp
 * @brief Vulkan render device implementation
 *
 * Implements the IDevice interface using Vulkan API for cross-platform
 * GPU-accelerated rendering. Provides full resource management, command
 * submission, and state setting capabilities.
 *
 * This implementation follows the RHI abstraction layer design:
 * - Vulkan handles are wrapped in opaque uint64_t handles
 * - Resource creation/destruction is explicit
 * - State setting is immediate (no deferred command recording at RHI level)
 */

#include "rhi_vulkan.h"
#include "rhi_types.h"

#ifdef _WIN32
    #ifndef VK_USE_PLATFORM_WIN32_KHR
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
    #include <windows.h>
#endif
#include <vulkan/vulkan.h>

#include <unordered_map>
#include <vector>
#include <array>
#include <cstring>
#include <cassert>

#include "Log/SyLogger.h"

namespace render::rhi
{
    // ---------------------------------------------------------------------------
    // Handle encoding scheme
    // ---------------------------------------------------------------------------
    // Handles are uint64_t values encoding both type and index:
    //   High 4 bits: type tag (Buffer=1, Texture=2, Pipeline=3)
    //   Low 60 bits: index into the corresponding resource array
    // This allows O(1) lookup and type-safe destruction.

    static constexpr uint64_t kTypeMask = 0xF000000000000000ULL;
    static constexpr uint64_t kIndexMask = 0x0FFFFFFFFFFFFFFFULL;
    static constexpr uint64_t kTypeShift = 60;

    static constexpr uint64_t kStagingBufferSize = 1 << 20;  // 1 MB staging fallback threshold

    static constexpr uint64_t kTypeBuffer = 1ULL << kTypeShift;
    static constexpr uint64_t kTypeTexture = 2ULL << kTypeShift;
    static constexpr uint64_t kTypePipeline = 3ULL << kTypeShift;

    static uint64_t makeHandle(uint64_t typeTag, uint64_t index)
    {
        return typeTag | (index & kIndexMask);
    }

    // ---------------------------------------------------------------------------
    // Internal resource structures (defined for VulkanDevice private members)
    // ---------------------------------------------------------------------------

    struct VulkanDevice::BufferResource
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryType memoryType = MemoryType::GPU_Only;
        bool isMapped = false;
        void* mappedPtr = nullptr;
        std::vector<uint8_t> stagingMemory;
    };

    struct VulkanDevice::TextureResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        Format format = Format::RGBA8;
        uint32_t mipLevels = 1;
    };

    struct VulkanDevice::PipelineResource
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        VertexFormat vertexFormat = VertexFormat::P3C3;
    };

    // ---------------------------------------------------------------------------
    // Vertex format to Vulkan conversion
    // ---------------------------------------------------------------------------

    static VkFormat vertexFormatToVulkan(VertexFormat fmt)
    {
        switch (fmt)
        {
        case VertexFormat::P3C3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3C4:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3N3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3T2:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3T2C4:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P2T2C4:
            return VK_FORMAT_R32G32_SFLOAT;
        default:
            return VK_FORMAT_R32G32B32_SFLOAT;
        }
    }

    static VkFormat vertexFormatColorToVulkan(VertexFormat fmt)
    {
        switch (fmt)
        {
        case VertexFormat::P3C3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3C4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexFormat::P3N3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexFormat::P3T2:
            return VK_FORMAT_R32G32_SFLOAT;
        case VertexFormat::P3T2C4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexFormat::P2T2C4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    static uint32_t vertexFormatStride(VertexFormat fmt)
    {
        switch (fmt)
        {
        case VertexFormat::P3C3:
            return 24;
        case VertexFormat::P3C4:
            return 28;
        case VertexFormat::P3N3:
            return 24;
        case VertexFormat::P3T2:
            return 20;
        case VertexFormat::P3T2C4:
            return 36;
        case VertexFormat::P2T2C4:
            return 32;
        default:
            return 0;
        }
    }

    // ---------------------------------------------------------------------------
    // Format mapping
    // ---------------------------------------------------------------------------

    static VkFormat toVulkanFormat(Format fmt)
    {
        switch (fmt)
        {
        case Format::RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA32F:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::RG32F:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32F:
            return VK_FORMAT_R32_SFLOAT;
        case Format::D32F:
            return VK_FORMAT_D32_SFLOAT;
        case Format::D24S8:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::R8:
            return VK_FORMAT_R8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    static VkPrimitiveTopology toVulkanTopology(PrimitiveTopology topo)
    {
        switch (topo)
        {
        case PrimitiveTopology::PointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::LineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::LineStrip:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::LineLoop:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::TriangleList:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::TriangleFan:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    static VkCompareOp toVulkanCompareOp(CompareFunc func)
    {
        switch (func)
        {
        case CompareFunc::Never:
            return VK_COMPARE_OP_NEVER;
        case CompareFunc::Less:
            return VK_COMPARE_OP_LESS;
        case CompareFunc::Equal:
            return VK_COMPARE_OP_EQUAL;
        case CompareFunc::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunc::Greater:
            return VK_COMPARE_OP_GREATER;
        case CompareFunc::Always:
            return VK_COMPARE_OP_ALWAYS;
        default:
            return VK_COMPARE_OP_ALWAYS;
        }
    }

    static VkBlendFactor toVulkanBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero:
            return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:
            return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        default:
            return VK_BLEND_FACTOR_ONE;
        }
    }

    // ---------------------------------------------------------------------------
    // Vulkan helper: find memory type
    // ---------------------------------------------------------------------------

    static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        SY_ERRORF("Vulkan: failed to find suitable memory type");
        return 0;
    }

    // ---------------------------------------------------------------------------
    // VulkanDevice implementation
    // ---------------------------------------------------------------------------

    VulkanDevice::VulkanDevice() = default;

    VulkanDevice::~VulkanDevice() = default;

    bool VulkanDevice::initialize(void* nativeWindow, uint32_t width, uint32_t height)
    {
        if (m_initialized)
        {
            SY_WARNF("VulkanDevice::initialize: already initialized");
            return false;
        }

        m_width = width;
        m_height = height;

#ifdef _WIN32
        // --- Create Vulkan instance ---
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "SanYiRender";
        appInfo.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
        appInfo.pEngineName = "SanYiRender";
        appInfo.engineVersion = VK_MAKE_VERSION(2, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        // Enable required extensions for Win32 surface
        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        if (m_debugLayer)
        {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (m_debugLayer)
        {
            createInfo.enabledLayerCount = 1;
            const char* debugLayers[] = { "VK_LAYER_KHRONOS_validation" };
            createInfo.ppEnabledLayerNames = debugLayers;
        }

        if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create Vulkan instance");
            return false;
        }

        // --- Create Win32 surface ---
        VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{};
        surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surfaceCreateInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceCreateInfo.hwnd = static_cast<HWND>(nativeWindow);

        PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR =
            (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(m_instance, "vkCreateWin32SurfaceKHR");

        if (!vkCreateWin32SurfaceKHR ||
            vkCreateWin32SurfaceKHR(m_instance, &surfaceCreateInfo, nullptr, &m_surface) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create Win32 surface");
            return false;
        }

        // --- Pick physical device ---
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            SY_ERRORF("VulkanDevice::initialize: no Vulkan-capable devices found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        // Select first discrete GPU or integrated GPU
        for (const auto& device : devices)
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            {
                m_physicalDevice = device;
                break;
            }
        }

        if (m_physicalDevice == VK_NULL_HANDLE)
        {
            // Fallback to first device
            m_physicalDevice = devices[0];
        }

        vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProperties);
        SY_DEBUGF("VulkanDevice: using device: %s", m_deviceProperties.deviceName);

        // --- Find queue families ---
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                m_graphicsQueueFamily = i;
            }

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, i, m_surface, &presentSupport);
            if (presentSupport)
            {
                m_presentQueueFamily = i;
            }
        }

        // --- Create logical device ---
        std::set<uint32_t> uniqueQueueFamilies;
        if (m_graphicsQueueFamily >= 0)
        {
            uniqueQueueFamilies.insert(m_graphicsQueueFamily);
        }
        if (m_presentQueueFamily >= 0)
        {
            uniqueQueueFamilies.insert(m_presentQueueFamily);
        }

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::vector<float> queuePriorities = { 1.0f };

        for (uint32_t queueFamily : uniqueQueueFamilies)
        {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = queuePriorities.data();
            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceFeatures deviceFeatures{};

        std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
        deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (m_debugLayer)
        {
            deviceCreateInfo.enabledLayerCount = 1;
            const char* debugLayers[] = { "VK_LAYER_KHRONOS_validation" };
            deviceCreateInfo.ppEnabledLayerNames = debugLayers;
        }

        if (vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create logical device");
            return false;
        }

        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);

        // --- Create command pool ---
        VkCommandPoolCreateInfo poolCreateInfo{};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(m_device, &poolCreateInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create command pool");
            return false;
        }

        // --- Create swapchain ---
        if (!createSwapchain(width, height))
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create swapchain");
            return false;
        }

        // --- Create descriptor pool for uniform buffers ---
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        };

        VkDescriptorPoolCreateInfo descriptorPoolInfo{};
        descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptorPoolInfo.poolSizeCount = 4;
        descriptorPoolInfo.pPoolSizes = poolSizes;
        descriptorPoolInfo.maxSets = 4096;

        if (vkCreateDescriptorPool(m_device, &descriptorPoolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create descriptor pool");
            return false;
        }

        // --- Create frame resources ---
        if (!createFrameResources())
        {
            SY_ERRORF("VulkanDevice::initialize: failed to create frame resources");
            return false;
        }

        m_initialized = true;
        SY_DEBUGF("VulkanDevice::initialize: success (queue families: gfx=%d, present=%d)",
            m_graphicsQueueFamily,
            m_presentQueueFamily);
        return true;
#else
        SY_ERRORF("VulkanDevice::initialize: Win32 platform not available");
        return false;
#endif
    }

    void VulkanDevice::shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        vkDeviceWaitIdle(m_device);

        // Destroy pipelines
        for (auto& [handle, res] : m_pipelines)
        {
            if (res->pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_device, res->pipeline, nullptr);
            }
            if (res->pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, res->pipelineLayout, nullptr);
            }
        }
        m_pipelines.clear();

        // Destroy textures
        for (auto& [handle, res] : m_textures)
        {
            if (res->sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(m_device, res->sampler, nullptr);
            }
            if (res->view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, res->view, nullptr);
            }
            if (res->image != VK_NULL_HANDLE)
            {
                vkDestroyImage(m_device, res->image, nullptr);
            }
            if (res->memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, res->memory, nullptr);
            }
        }
        m_textures.clear();

        // Destroy buffers
        for (auto& [handle, res] : m_buffers)
        {
            if (res->buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, res->buffer, nullptr);
            }
            if (res->memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, res->memory, nullptr);
            }
        }
        m_buffers.clear();

        // Destroy frame resources
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (m_inFlightFences[i] != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
            }
            if (m_commandBuffers[i] != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &m_commandBuffers[i]);
            }
        }
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);

        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);

        // Destroy swapchain
        for (auto& framebuffer : m_swapchainFramebuffers)
        {
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);
        }
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        for (auto& imageView : m_swapchainImageViews)
        {
            vkDestroyImageView(m_device, imageView, nullptr);
        }
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);

        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        vkDestroyDevice(m_device, nullptr);
        if (m_debugLayer)
        {
            // Would destroy debug messenger if enabled
        }
        vkDestroyInstance(m_instance, nullptr);

        m_initialized = false;
        SY_DEBUGF("VulkanDevice::shutdown: complete");
    }

    bool VulkanDevice::createSwapchain(uint32_t width, uint32_t height)
    {
        // Query swapchain support
        SwapchainSupportDetails details = querySwapchainSupport(m_physicalDevice, m_surface);

        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(details.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(details.presentModes);
        VkExtent2D extent = chooseSwapExtent(details.capabilities, width, height);

        uint32_t imageCount = details.capabilities.minImageCount + 1;
        if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount)
        {
            imageCount = details.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        uint32_t queueFamilyIndices[] = { static_cast<uint32_t>(m_graphicsQueueFamily),
            static_cast<uint32_t>(m_presentQueueFamily) };
        if (m_graphicsQueueFamily != m_presentQueueFamily)
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = details.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS)
        {
            return false;
        }

        // Retrieve swapchain images
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
        m_swapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

        m_swapchainImageFormat = surfaceFormat.format;
        m_swapchainExtent = extent;

        // Create image views
        m_swapchainImageViews.resize(m_swapchainImages.size());
        for (size_t i = 0; i < m_swapchainImages.size(); i++)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_swapchainImageFormat;
            viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS)
            {
                SY_ERRORF("VulkanDevice: failed to create swapchain image view");
                return false;
            }
        }

        // Create render pass
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_swapchainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkPipelineRasterizationStateCreateInfo rasterizerInfo{};
        rasterizerInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizerInfo.depthClampEnable = VK_FALSE;
        rasterizerInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizerInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizerInfo.lineWidth = 1.0f;
        rasterizerInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizerInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizerInfo.depthBiasEnable = VK_FALSE;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_defaultPipelineLayout) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice: failed to create default pipeline layout");
            return false;
        }

        VkPipelineCacheCreateInfo pipelineCacheInfo{};
        pipelineCacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice: failed to create pipeline cache");
            return false;
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        // Create framebuffers
        m_swapchainFramebuffers.resize(m_swapchainImageViews.size());
        for (size_t i = 0; i < m_swapchainImageViews.size(); i++)
        {
            VkImageView attachments[] = { m_swapchainImageViews[i] };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = m_swapchainExtent.width;
            framebufferInfo.height = m_swapchainExtent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_swapchainFramebuffers[i]) != VK_SUCCESS)
            {
                SY_ERRORF("VulkanDevice: failed to create framebuffer");
                return false;
            }
        }

        return true;
    }

    bool VulkanDevice::createFrameResources()
    {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        VkCommandPool commandPool;
        commandPool = m_commandPool;  // Already created

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
            {
                SY_ERRORF("VulkanDevice: failed to create synchronization fence");
                return false;
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;

            if (vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffers[i]) != VK_SUCCESS)
            {
                SY_ERRORF("VulkanDevice: failed to allocate command buffers");
                return false;
            }
        }

        m_commandBufferIndex = 0;
        return true;
    }

    VulkanDevice::SwapchainSupportDetails VulkanDevice::querySwapchainSupport(
        VkPhysicalDevice device, VkSurfaceKHR surface)
    {
        SwapchainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
        if (formatCount != 0)
        {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
        if (presentModeCount != 0)
        {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    VkSurfaceFormatKHR VulkanDevice::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available)
    {
        for (const auto& format : available)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                return format;
            }
        }
        return available[0];
    }

    VkPresentModeKHR VulkanDevice::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& available)
    {
        for (const auto& presentMode : available)
        {
            if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
            {
                return presentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanDevice::chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
    {
        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            return capabilities.currentExtent;
        }
        else
        {
            VkExtent2D actualExtent = { width, height };
            actualExtent.width = std::max(
                capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, actualExtent.width));
            actualExtent.height = std::max(
                capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, actualExtent.height));
            return actualExtent;
        }
    }

    BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc)
    {
        auto res = std::make_unique<BufferResource>();
        res->size = static_cast<VkDeviceSize>(desc.size);
        res->usage = desc.usage;
        res->memoryType = desc.memory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = desc.size;

        VkBufferUsageFlags usageFlags = 0;
        switch (desc.usage)
        {
        case BufferUsage::Vertex:
            usageFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case BufferUsage::Index:
            usageFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case BufferUsage::Uniform:
            usageFlags = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        case BufferUsage::Indirect:
            usageFlags = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
            break;
        case BufferUsage::ShaderVisible:
            usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            break;
        case BufferUsage::Staging:
            usageFlags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            break;
        case BufferUsage::ShaderStorage:
            usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            break;
        }

        // Always include transfer dst for uploads
        usageFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        bufferInfo.usage = usageFlags;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &res->buffer) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createBuffer: failed");
            return NullHandle;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, res->buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice,
            memRequirements.memoryTypeBits,
            (desc.memory == MemoryType::CPU_Visible || desc.memory == MemoryType::GPU_CPU_Coherent)
                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &res->memory) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createBuffer: failed to allocate memory");
            return NullHandle;
        }

        vkBindBufferMemory(m_device, res->buffer, res->memory, 0);

        BufferHandle handle = makeHandle(kTypeBuffer, m_nextBufferId++);
        m_buffers[handle] = std::move(res);
        return handle;
    }

    void VulkanDevice::destroyBuffer(BufferHandle handle)
    {
        auto it = m_buffers.find(handle);
        if (it != m_buffers.end())
        {
            if (it->second->buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, it->second->buffer, nullptr);
            }
            if (it->second->memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, it->second->memory, nullptr);
            }
            m_buffers.erase(it);
        }
    }

    TextureHandle VulkanDevice::createTexture(const TextureDesc& desc)
    {
        auto res = std::make_unique<TextureResource>();
        res->width = desc.width;
        res->height = desc.height;
        res->format = desc.format;
        res->mipLevels = desc.mipLevels;

        VkFormat vkFormat = toVulkanFormat(desc.format);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = desc.width;
        imageInfo.extent.height = desc.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = vkFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (desc.format == Format::D32F || desc.format == Format::D24S8)
        {
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        else
        {
            imageInfo.usage =
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &res->image) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createTexture: failed to create image");
            return NullHandle;
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, res->image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &res->memory) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createTexture: failed to allocate memory");
            return NullHandle;
        }

        vkBindImageMemory(m_device, res->image, res->memory, 0);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = res->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = vkFormat;
        viewInfo.subresourceRange.aspectMask = (desc.format == Format::D32F) ? VK_IMAGE_ASPECT_DEPTH_BIT
            : (desc.format == Format::D24S8) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                             : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = desc.mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &res->view) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createTexture: failed to create image view");
            return NullHandle;
        }

        // Create sampler for color formats
        if (desc.format != Format::D32F && desc.format != Format::D24S8)
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.anisotropyEnable = VK_FALSE;
            samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            samplerInfo.unnormalizedCoordinates = VK_FALSE;
            samplerInfo.compareEnable = VK_FALSE;
            samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.mipLodBias = 0.0f;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = static_cast<float>(desc.mipLevels);

            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &res->sampler) != VK_SUCCESS)
            {
                SY_WARNF("VulkanDevice::createTexture: failed to create sampler");
            }
        }

        TextureHandle handle = makeHandle(kTypeTexture, m_nextTextureId++);
        m_textures[handle] = std::move(res);
        return handle;
    }

    void VulkanDevice::destroyTexture(TextureHandle handle)
    {
        auto it = m_textures.find(handle);
        if (it != m_textures.end())
        {
            if (it->second->sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(m_device, it->second->sampler, nullptr);
            }
            if (it->second->view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, it->second->view, nullptr);
            }
            if (it->second->image != VK_NULL_HANDLE)
            {
                vkDestroyImage(m_device, it->second->image, nullptr);
            }
            if (it->second->memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, it->second->memory, nullptr);
            }
            m_textures.erase(it);
        }
    }

    PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc)
    {
        auto res = std::make_unique<PipelineResource>();

        // Create shader stages
        // Note: This assumes shader source is SPIR-V compiled
        // In production, would need shader compiler integration (glslang/shaderc)
        if (!m_shaderStages.empty())
        {
            // Shaders already loaded into pipeline
        }

        // Create vertex input state
        VkVertexInputBindingDescription vertexBinding{};
        vertexBinding.binding = 0;
        vertexBinding.stride = vertexFormatStride(desc.vertexFormat);
        vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        uint32_t offset = 0;

        // Position attribute (always first)
        VkVertexInputAttributeDescription posAttr{};
        posAttr.binding = 0;
        posAttr.location = 0;
        posAttr.offset = 0;
        posAttr.format = vertexFormatToVulkan(desc.vertexFormat);
        vertexAttributes.push_back(posAttr);
        offset += (desc.vertexFormat == VertexFormat::P2T2C4) ? 8 : 12;

        // Color attribute
        VkVertexInputAttributeDescription colorAttr{};
        colorAttr.binding = 0;
        colorAttr.location = 1;
        colorAttr.offset = offset;
        colorAttr.format = vertexFormatColorToVulkan(desc.vertexFormat);
        if (colorAttr.format != VK_FORMAT_UNDEFINED)
        {
            vertexAttributes.push_back(colorAttr);
        }
        offset += (colorAttr.format == VK_FORMAT_UNDEFINED)                                            ? 0
            : (desc.vertexFormat == VertexFormat::P2T2C4 || desc.vertexFormat == VertexFormat::P3T2C4) ? 16
                                                                                                       : 12;

        // Normal or TexCoord attribute
        if (offset > 0)
        {
            VkVertexInputAttributeDescription attr{};
            attr.binding = 0;
            attr.location = 2;
            attr.offset = offset;
            attr.format = vertexFormatColorToVulkan(desc.vertexFormat);  // Simplified: use same format for other attrs
            if (attr.format != VK_FORMAT_UNDEFINED)
            {
                vertexAttributes.push_back(attr);
            }
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &vertexBinding;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
        vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes.data();

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = toVulkanTopology(desc.topology);
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport state
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_swapchainExtent.width);
        viewport.height = static_cast<float>(m_swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissorRect{};
        scissorRect.offset = { 0, 0 };
        scissorRect.extent = { m_swapchainExtent.width, m_swapchainExtent.height };

        VkPipelineViewportStateCreateInfo viewportStateInfo{};
        viewportStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportStateInfo.viewportCount = 1;
        viewportStateInfo.pViewports = &viewport;
        viewportStateInfo.scissorCount = 1;
        viewportStateInfo.pScissors = &scissorRect;

        // Rasterization state
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // Multisampling (disabled)
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Color blending
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = toVulkanBlendFactor(desc.srcBlend);
        colorBlendAttachment.dstColorBlendFactor = toVulkanBlendFactor(desc.dstBlend);
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = toVulkanBlendFactor(desc.srcBlend);
        colorBlendAttachment.dstAlphaBlendFactor = toVulkanBlendFactor(desc.dstBlend);
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // Pipeline layout
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 0;
        pipelineLayoutInfo.pSetLayouts = nullptr;
        pipelineLayoutInfo.pushConstantRangeCount = 0;
        pipelineLayoutInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &res->pipelineLayout) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createPipeline: failed to create pipeline layout");
            return NullHandle;
        }

        // Graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = static_cast<uint32_t>(m_shaderStages.size());
        pipelineInfo.pStages = m_shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportStateInfo;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = res->pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pipelineInfo, nullptr, &res->pipeline) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::createPipeline: failed to create graphics pipeline");
            return NullHandle;
        }

        res->topology = desc.topology;
        res->vertexFormat = desc.vertexFormat;

        PipelineHandle handle = makeHandle(kTypePipeline, m_nextPipelineId++);
        m_pipelines[handle] = std::move(res);
        return handle;
    }

    void VulkanDevice::destroyPipeline(PipelineHandle handle)
    {
        auto it = m_pipelines.find(handle);
        if (it != m_pipelines.end())
        {
            if (it->second->pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_device, it->second->pipeline, nullptr);
            }
            if (it->second->pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, it->second->pipelineLayout, nullptr);
            }
            m_pipelines.erase(it);
        }
    }

    void VulkanDevice::uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t size, const void* data)
    {
        if (!data || size == 0)
        {
            return;
        }

        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::uploadBuffer: invalid buffer handle");
            return;
        }

        auto& res = it->second;

        if (res->memoryType == MemoryType::CPU_Visible || res->memoryType == MemoryType::GPU_CPU_Coherent)
        {
            // Direct mapping
            void* mappedPtr = nullptr;
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = res->memory;
            range.offset = offset;
            range.size = size;

            if (vkMapMemory(m_device, res->memory, offset, size, 0, &mappedPtr) == VK_SUCCESS)
            {
                std::memcpy(static_cast<char*>(mappedPtr), data, size);
                vkFlushMappedMemoryRanges(m_device, 1, &range);
                vkUnmapMemory(m_device, res->memory);
            }
        }
        else
        {
            // Use staging buffer for GPU-only resources
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingBufferMemory;

            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = size;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer);

            VkMemoryRequirements memRequirements;
            vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice,
                memRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingBufferMemory);
            vkBindBufferMemory(m_device, stagingBuffer, stagingBufferMemory, 0);

            void* mappedPtr = nullptr;
            vkMapMemory(m_device, stagingBufferMemory, 0, size, 0, &mappedPtr);
            std::memcpy(mappedPtr, data, size);
            vkUnmapMemory(m_device, stagingBufferMemory);

            VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            vkBeginCommandBuffer(commandBuffer, &beginInfo);

            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = offset;
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, stagingBuffer, res->buffer, 1, &copyRegion);

            vkEndCommandBuffer(commandBuffer);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;

            vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_graphicsQueue);

            vkDestroyBuffer(m_device, stagingBuffer, nullptr);
            vkFreeMemory(m_device, stagingBufferMemory, nullptr);
        }
    }

    void VulkanDevice::uploadTexture(TextureHandle handle, uint32_t mip, const void* data, uint32_t rowPitch)
    {
        auto it = m_textures.find(handle);
        if (it == m_textures.end())
        {
            SY_ERRORF("VulkanDevice::uploadTexture: invalid texture handle");
            return;
        }

        auto& res = it->second;
        uint32_t mipWidth = std::max(1u, res->width >> mip);
        uint32_t mipHeight = std::max(1u, res->height >> mip);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = static_cast<VkDeviceSize>(rowPitch) * mipHeight;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice,
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingBufferMemory);
        vkBindBufferMemory(m_device, stagingBuffer, stagingBufferMemory, 0);

        void* mappedPtr = nullptr;
        vkMapMemory(m_device, stagingBufferMemory, 0, memRequirements.size, 0, &mappedPtr);
        std::memcpy(mappedPtr, data, memRequirements.size);
        vkUnmapMemory(m_device, stagingBufferMemory);

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { mipWidth, mipHeight, 1 };

        vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, res->image, VK_IMAGE_LAYOUT_UNDEFINED, 1, &region);

        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
    }

    void* VulkanDevice::mapBuffer(BufferHandle handle, uint64_t offset, uint64_t size, uint32_t /*mapFlags*/)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::mapBuffer: invalid buffer handle");
            return nullptr;
        }

        auto& res = it->second;
        if (res->memoryType == MemoryType::CPU_Visible || res->memoryType == MemoryType::GPU_CPU_Coherent)
        {
            void* mappedPtr = nullptr;
            if (vkMapMemory(m_device, res->memory, offset, size, 0, &mappedPtr) == VK_SUCCESS)
            {
                res->isMapped = true;
                res->mappedPtr = mappedPtr;
                return mappedPtr;
            }
        }

        // Fall back to temporary staging for GPU-only buffers
        if (!res->stagingMemory.empty() || size <= kStagingBufferSize)
        {
            if (res->stagingMemory.size() < size)
            {
                res->stagingMemory.resize(size);
            }
            return res->stagingMemory.data();
        }

        return nullptr;
    }

    void VulkanDevice::unmapBuffer(BufferHandle handle)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            return;
        }

        auto& res = it->second;
        if (res->isMapped &&
            (res->memoryType == MemoryType::CPU_Visible || res->memoryType == MemoryType::GPU_CPU_Coherent))
        {
            vkUnmapMemory(m_device, res->memory);
            res->isMapped = false;
            res->mappedPtr = nullptr;
        }
    }

    void VulkanDevice::flushMappedRange(BufferHandle handle, uint64_t offset, uint64_t size)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            return;
        }

        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = it->second->memory;
        range.offset = offset;
        range.size = size;

        vkFlushMappedMemoryRanges(m_device, 1, &range);
    }

    void VulkanDevice::beginFrame()
    {
        vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

        vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &m_currentImageIndex);

        m_commandBufferIndex = m_currentFrame;

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkResetCommandBuffer(commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        // Begin render pass
        VkClearValue clearColor = {};
        clearColor.color = { m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3] };

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_renderPass;
        renderPassInfo.framebuffer = m_swapchainFramebuffers[m_currentImageIndex];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = m_swapchainExtent;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_boundPipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_swapchainExtent.width);
        viewport.height = static_cast<float>(m_swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissorRect{};
        scissorRect.offset = { 0, 0 };
        scissorRect.extent = { m_swapchainExtent.width, m_swapchainExtent.height };
        vkCmdSetScissor(commandBuffer, 0, 1, &scissorRect);
    }

    void VulkanDevice::endFrame()
    {
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdEndRenderPass(commandBuffer);
        vkEndCommandBuffer(commandBuffer);

        VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
        VkSemaphore renderFinishedSemaphores[MAX_FRAMES_IN_FLIGHT];

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        // Submit command buffer
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[m_currentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[m_currentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]) != VK_SUCCESS)
        {
            SY_ERRORF("VulkanDevice::endFrame: failed to submit draw command buffer");
        }

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapchains[] = { m_swapchain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &m_currentImageIndex;

        vkQueuePresentKHR(m_presentQueue, &presentInfo);

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
        m_commandBufferIndex = m_currentFrame;
    }

    void VulkanDevice::present()
    {
        // Presentation is handled in endFrame() for triple buffering
    }

    void VulkanDevice::bindPipeline(PipelineHandle handle)
    {
        auto it = m_pipelines.find(handle);
        if (it == m_pipelines.end())
        {
            SY_ERRORF("VulkanDevice::bindPipeline: invalid pipeline handle");
            return;
        }

        m_boundPipeline = it->second->pipeline;
        m_pipelineLayout = it->second->pipelineLayout;

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_boundPipeline);

        // Set default line width
        vkCmdSetLineWidth(commandBuffer, m_lineWidth);
    }

    void VulkanDevice::bindVertexBuffer(uint32_t slot, BufferHandle handle, uint64_t offset)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::bindVertexBuffer: invalid buffer handle");
            return;
        }

        VkBuffer vertexBuffers[] = { it->second->buffer };
        VkDeviceSize offsets[] = { static_cast<VkDeviceSize>(offset) };

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindVertexBuffers(commandBuffer, slot, 1, vertexBuffers, offsets);
    }

    void VulkanDevice::bindIndexBuffer(BufferHandle handle, uint64_t offset)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::bindIndexBuffer: invalid buffer handle");
            return;
        }

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindIndexBuffer(commandBuffer, it->second->buffer, offset, VK_INDEX_TYPE_UINT32);
    }

    void VulkanDevice::bindUniformBuffer(
        uint32_t set, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::bindUniformBuffer: invalid buffer handle");
            return;
        }

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, set, 1, &m_descriptorSets[set], 0, nullptr);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = it->second->buffer;
        bufferInfo.offset = static_cast<VkDeviceSize>(offset);
        bufferInfo.range = static_cast<VkDeviceSize>(size);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSets[set];
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }

    void VulkanDevice::bindShaderStorageBuffer(
        uint32_t set, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size)
    {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::bindShaderStorageBuffer: invalid buffer handle");
            return;
        }

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, set, 1, &m_descriptorSets[set], 0, nullptr);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = it->second->buffer;
        bufferInfo.offset = static_cast<VkDeviceSize>(offset);
        bufferInfo.range = static_cast<VkDeviceSize>(size);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSets[set];
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }

    void VulkanDevice::bindTexture(uint32_t set, uint32_t binding, TextureHandle handle)
    {
        auto it = m_textures.find(handle);
        if (it == m_textures.end())
        {
            SY_ERRORF("VulkanDevice::bindTexture: invalid texture handle");
            return;
        }

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, set, 1, &m_descriptorSets[set], 0, nullptr);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = it->second->view;
        imageInfo.sampler = it->second->sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSets[set];
        descriptorWrite.dstBinding = binding;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    }

    void VulkanDevice::setViewport(const Viewport& vp)
    {
        VkViewport viewport{};
        viewport.x = vp.x;
        viewport.y = vp.y;
        viewport.width = vp.w;
        viewport.height = vp.h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    }

    void VulkanDevice::setScissor(const Scissor& sc)
    {
        VkRect2D scissorRect{};
        scissorRect.offset = { sc.x, sc.y };
        scissorRect.extent = { sc.w, sc.h };

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdSetScissor(commandBuffer, 0, 1, &scissorRect);
    }

    void VulkanDevice::setLineWidth(float width)
    {
        m_lineWidth = width;
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdSetLineWidth(commandBuffer, width);
    }

    void VulkanDevice::setUniformMatrix3(const char* name, const float* data)
    {
        // Vulkan uses uniform buffers or push constants, not glUniform*-style calls
        // This is a simplified implementation that maps to a push constant
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        // Copy 3x3 matrix (9 floats) into push constants
        if (0 + 9 <= m_pushConstants.size())
        {
            std::memcpy(m_pushConstants.data() + 0, data, 9 * sizeof(float));
        }
    }

    void VulkanDevice::setUniformMatrix4(const char* /*name*/, const float* data)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        if (0 + 16 <= m_pushConstants.size())
        {
            std::memcpy(m_pushConstants.data() + 0, data, 16 * sizeof(float));
        }
    }

    void VulkanDevice::setUniformFloat(const char* name, float value)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        // Set at a known offset (simplified)
        m_pushConstants[16] = value;
    }

    void VulkanDevice::setUniformInt(const char* /*name*/, int32_t value)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        m_pushConstants[17] = static_cast<float>(value);
    }

    void VulkanDevice::setUniformVec2(const char* /*name*/, const float* data)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        if (18 + 2 <= m_pushConstants.size())
        {
            std::memcpy(m_pushConstants.data() + 18, data, 2 * sizeof(float));
        }
    }

    void VulkanDevice::setUniformVec3(const char* /*name*/, const float* data)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        if (20 + 3 <= m_pushConstants.size())
        {
            std::memcpy(m_pushConstants.data() + 20, data, 3 * sizeof(float));
        }
    }

    void VulkanDevice::setUniformVec4(const char* /*name*/, const float* data)
    {
        if (m_pushConstants.empty())
        {
            m_pushConstants.resize(256, 0.0f);
        }
        if (24 + 4 <= m_pushConstants.size())
        {
            std::memcpy(m_pushConstants.data() + 24, data, 4 * sizeof(float));
        }
    }

    void VulkanDevice::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        m_drawCallCount += instanceCount > 0 ? instanceCount : 1;
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VulkanDevice::drawIndexed(
        uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        m_drawCallCount += instanceCount > 0 ? instanceCount : 1;
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void VulkanDevice::drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        auto it = m_buffers.find(indirectBuffer);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::drawIndirect: invalid buffer handle");
            return;
        }

        m_drawCallCount += drawCount;
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdDrawIndirect(commandBuffer, it->second->buffer, offset, drawCount, stride);
    }

    void VulkanDevice::drawIndexedIndirect(
        BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        auto it = m_buffers.find(indirectBuffer);
        if (it == m_buffers.end())
        {
            SY_ERRORF("VulkanDevice::drawIndexedIndirect: invalid buffer handle");
            return;
        }

        m_drawCallCount += drawCount;
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdDrawIndexedIndirect(commandBuffer, it->second->buffer, offset, drawCount, stride);
    }

    void VulkanDevice::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdDispatch(commandBuffer, groupsX, groupsY, groupsZ);
    }

    void VulkanDevice::memoryBarrier(uint32_t barrierFlags)
    {
        VkMemoryBarrier memoryBarrierInfo{};
        memoryBarrierInfo.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrierInfo.srcAccessMask = static_cast<VkAccessFlags>(barrierFlags);
        memoryBarrierInfo.dstAccessMask = static_cast<VkAccessFlags>(barrierFlags);

        VkCommandBuffer commandBuffer = m_commandBuffers[m_commandBufferIndex];
        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            1,
            &memoryBarrierInfo,
            0,
            nullptr,
            0,
            nullptr);
    }

    void VulkanDevice::setClearColor(float r, float g, float b, float a)
    {
        m_clearColor[0] = r;
        m_clearColor[1] = g;
        m_clearColor[2] = b;
        m_clearColor[3] = a;
    }

    void VulkanDevice::clear(uint32_t /*flags*/)
    {
        // Clear is handled in beginFrame via render pass load ops
        // This is a no-op since we already clear at the start of the render pass
    }

    void VulkanDevice::enableDepthTest(bool enable)
    {
        // Depth test state is baked into pipeline; would need pipeline switch
        m_depthTestEnabled = enable;
    }

    void VulkanDevice::enableBlend(bool enable)
    {
        // Blend state is baked into pipeline; would need pipeline switch
        m_blendEnabled = enable;
    }

    void VulkanDevice::resize(uint32_t width, uint32_t height)
    {
        if (!m_initialized)
        {
            return;
        }

        vkDeviceWaitIdle(m_device);

        m_width = width;
        m_height = height;

        // TODO: Recreate swapchain and frame resources
        createSwapchain(width, height);
    }

    uint64_t VulkanDevice::getGPUMemoryUsage() const
    {
        uint64_t totalSize = 0;
        for (const auto& [handle, res] : m_buffers)
        {
            totalSize += res->size;
        }
        for (const auto& [handle, res] : m_textures)
        {
            VkMemoryRequirements reqs;
            vkGetImageMemoryRequirements(m_device, res->image, &reqs);
            totalSize += reqs.size;
        }
        return totalSize;
    }

    void* VulkanDevice::getNativeContext()
    {
        return static_cast<void*>(m_device);
    }

    // Factory function
    IDevice* createVulkanDevice()
    {
        return new VulkanDevice();
    }
}  // namespace render::rhi