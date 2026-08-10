/**
 * @file rhi_device.h
 * @brief Render Hardware Interface (RHI) 设备接口定义
 * 
 * 本文件定义了 RHI 层的核心设备接口 IDevice，提供了跨平台的图形硬件抽象。
 * 所有图形后端（OpenGL、Vulkan、Metal等）都需要实现此接口。
 * 
 * 接口设计遵循以下原则：
 * - 命令式 API，与传统图形API风格一致
 * - 资源管理明确（创建/销毁配对）
 * - 状态设置与绘制分离
 * - 支持间接绘制和实例化渲染
 */
#pragma once
#include "rhi_types.h"
#include <cstddef>

namespace render::rhi {

/**
 * @brief 渲染设备接口类
 * 
 * 定义了图形渲染的核心操作，包括：
 * - 设备初始化和关闭
 * - 资源创建和销毁（缓冲区、纹理、管线）
 * - 数据上传和映射
 * - 状态设置（管线绑定、顶点缓冲区绑定、uniform设置等）
 * - 绘制命令
 * - 帧管理
 */
class IDevice {
public:
    virtual ~IDevice() = default;

    /**
     * @brief 初始化渲染设备
     * 
     * @param nativeWindow 原生窗口句柄（如HWND）
     * @param width 渲染目标宽度
     * @param height 渲染目标高度
     * @return 初始化是否成功
     */
    virtual bool initialize(void* nativeWindow, uint32_t width, uint32_t height) = 0;

    /**
     * @brief 关闭渲染设备并释放所有资源
     */
    virtual void shutdown() = 0;

    /// @name 资源创建与销毁
    /// @{

    /**
     * @brief 创建缓冲区
     * 
     * @param desc 缓冲区描述
     * @return 缓冲区句柄，失败返回NullHandle
     */
    virtual BufferHandle   createBuffer(const BufferDesc&) = 0;

    /**
     * @brief 销毁缓冲区
     * 
     * @param handle 要销毁的缓冲区句柄
     */
    virtual void           destroyBuffer(BufferHandle) = 0;

    /**
     * @brief 创建纹理
     * 
     * @param desc 纹理描述
     * @return 纹理句柄，失败返回NullHandle
     */
    virtual TextureHandle  createTexture(const TextureDesc&) = 0;

    /**
     * @brief 销毁纹理
     * 
     * @param handle 要销毁的纹理句柄
     */
    virtual void           destroyTexture(TextureHandle) = 0;

    /**
     * @brief 创建图形管线
     * 
     * @param desc 管线描述
     * @return 管线句柄，失败返回NullHandle
     */
    virtual PipelineHandle createPipeline(const PipelineDesc&) = 0;

    /**
     * @brief 销毁图形管线
     * 
     * @param handle 要销毁的管线句柄
     */
    virtual void           destroyPipeline(PipelineHandle) = 0;
    /// @}

    /// @name 数据上传与映射
    /// @{

    /**
     * @brief 上传数据到缓冲区
     * 
     * @param handle 缓冲区句柄
     * @param offset 目标偏移量（字节）
     * @param size 数据大小（字节）
     * @param data 数据源指针
     */
    virtual void uploadBuffer(BufferHandle, uint64_t offset, uint64_t size, const void* data) = 0;

    /**
     * @brief 上传数据到纹理
     * 
     * @param handle 纹理句柄
     * @param mip MIP级别
     * @param data 像素数据指针
     * @param rowPitch 每行字节数
     */
    virtual void uploadTexture(TextureHandle, uint32_t mip, const void* data, uint32_t rowPitch) = 0;

    /**
     * @brief 映射缓冲区到CPU可访问内存
     * 
     * @param handle 缓冲区句柄
     * @param offset 映射偏移量（字节）
     * @param size 映射大小（字节）
     * @param mapFlags 映射标志
     * @return 映射后的内存指针，失败返回nullptr
     */
    virtual void* mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t mapFlags) = 0;

    /**
     * @brief 取消缓冲区映射
     * 
     * @param handle 缓冲区句柄
     */
    virtual void unmapBuffer(BufferHandle) = 0;

    /**
     * @brief 刷新映射的内存区域（确保GPU可见）
     * 
     * @param handle 缓冲区句柄
     * @param offset 偏移量（字节）
     * @param size 大小（字节）
     */
    virtual void flushMappedRange(BufferHandle, uint64_t offset, uint64_t size) = 0;
    /// @}

    /// @name 帧管理
    /// @{

    /**
     * @brief 开始新帧
     * 
     * 调用此方法后可以开始提交绘制命令。
     */
    virtual void beginFrame() = 0;

    /**
     * @brief 结束当前帧
     * 
     * 调用此方法后完成帧的命令提交。
     */
    virtual void endFrame() = 0;

    /**
     * @brief 呈现当前帧到窗口
     * 
     * 将后台缓冲区的内容显示到窗口。
     */
    virtual void present() = 0;
    /// @}

    /// @name 状态设置
    /// @{

    /**
     * @brief 绑定图形管线
     * 
     * @param handle 管线句柄
     */
    virtual void bindPipeline(PipelineHandle) = 0;

    /**
     * @brief 绑定顶点缓冲区
     * 
     * @param slot 绑定槽位（0-3）
     * @param handle 缓冲区句柄
     * @param offset 偏移量（字节）
     */
    virtual void bindVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) = 0;

    /**
     * @brief 绑定索引缓冲区
     * 
     * @param handle 缓冲区句柄
     * @param offset 偏移量（字节）
     */
    virtual void bindIndexBuffer(BufferHandle, uint64_t offset) = 0;

    /**
     * @brief 绑定统一缓冲区（Uniform Buffer）
     * 
     * @param set 描述符集索引
     * @param binding 绑定点索引
     * @param handle 缓冲区句柄
     * @param offset 偏移量（字节）
     * @param size 大小（字节）
     */
    virtual void bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) = 0;

    /**
     * @brief 绑定着色器存储缓冲区（SSBO）
     *
     * 用于 OpenGL 的 Shader Storage Buffer Object（GL 4.3+）。
     * SSBO 容量远大于 UBO，支持随机读写。
     *
     * @param set 描述符集索引
     * @param binding 绑定点索引
     * @param handle 缓冲区句柄
     * @param offset 偏移量（字节）
     * @param size 大小（字节）
     */
    virtual void bindShaderStorageBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) = 0;

    /**
     * @brief 绑定纹理
     * 
     * @param set 描述符集索引
     * @param binding 绑定点索引
     * @param handle 纹理句柄
     */
    virtual void bindTexture(uint32_t set, uint32_t binding, TextureHandle) = 0;

    /**
     * @brief 设置视口
     * 
     * @param viewport 视口参数
     */
    virtual void setViewport(const Viewport&) = 0;

    /**
     * @brief 设置裁剪矩形
     * 
     * @param scissor 裁剪矩形参数
     */
    virtual void setScissor(const Scissor&) = 0;

    /**
     * @brief 设置线宽
     * 
     * @param width 线宽（像素）
     */
    virtual void setLineWidth(float width) = 0;
    /// @}

    /// @name Uniform 设置
    /// @{

    /**
     * @brief 设置3x3矩阵uniform
     * 
     * @param name uniform名称
     * @param data 矩阵数据（9个float，列主序）
     */
    virtual void setUniformMatrix3(const char* name, const float* data) = 0;

    /**
     * @brief 设置4x4矩阵uniform
     * 
     * @param name uniform名称
     * @param data 矩阵数据（16个float，列主序）
     */
    virtual void setUniformMatrix4(const char* name, const float* data) = 0;

    /**
     * @brief 设置float类型uniform
     *
     * @param name uniform名称
     * @param value float值
     */
    virtual void setUniformFloat(const char* name, float value) = 0;

    /**
     * @brief 设置int类型uniform
     *
     * @param name uniform名称
     * @param value int值
     */
    virtual void setUniformInt(const char* name, int32_t value) = 0;

    /**
     * @brief 设置2D向量uniform
     * 
     * @param name uniform名称
     * @param data 向量数据（2个float）
     */
    virtual void setUniformVec2(const char* name, const float* data) = 0;

    /**
     * @brief 设置3D向量uniform
     * 
     * @param name uniform名称
     * @param data 向量数据（3个float）
     */
    virtual void setUniformVec3(const char* name, const float* data) = 0;

    /**
     * @brief 设置4D向量uniform
     * 
     * @param name uniform名称
     * @param data 向量数据（4个float）
     */
    virtual void setUniformVec4(const char* name, const float* data) = 0;
    /// @}

    /// @name 绘制命令
    /// @{

    /**
     * @brief 执行非索引绘制
     * 
     * @param vertexCount 顶点数量
     * @param instanceCount 实例数量
     * @param firstVertex 起始顶点索引
     * @param firstInstance 起始实例索引
     */
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;

    /**
     * @brief 执行索引绘制
     * 
     * @param indexCount 索引数量
     * @param instanceCount 实例数量
     * @param firstIndex 起始索引
     * @param vertexOffset 顶点偏移
     * @param firstInstance 起始实例索引
     */
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;

    /**
     * @brief 执行间接绘制
     * 
     * @param indirectBuffer 间接命令缓冲区
     * @param offset 命令偏移量（字节）
     * @param drawCount 绘制次数
     * @param stride 命令步长（字节）
     */
    virtual void drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;

    /**
     * @brief 执行索引间接绘制
     * 
     * @param indirectBuffer 间接命令缓冲区
     * @param offset 命令偏移量（字节）
     * @param drawCount 绘制次数
     * @param stride 命令步长（字节）
     */
    virtual void drawIndexedIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;

    /**
     * @brief 分发计算着色器
     *
     * 执行 GL 4.3+ 的 Compute Shader 工作组分发。
     * 调用后如需读取写入的缓冲区，应插入适当的内存屏障。
     *
     * @param groupsX X 方向工作组数量
     * @param groupsY Y 方向工作组数量
     * @param groupsZ Z 方向工作组数量
     */
    virtual void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

    /**
     * @brief 插入内存屏障
     *
     * 确保在此调用之前的 GPU 写入对后续操作可见。
     * 常用于 Compute Shader 写入缓冲区后的同步点。
     *
     * @param barrierFlags 屏障标志位组合
     */
    virtual void memoryBarrier(uint32_t barrierFlags) = 0;
    /// @}

    /// @name 渲染状态
    /// @{

    /**
     * @brief 设置清除颜色
     * 
     * @param r 红色分量（0-1）
     * @param g 绿色分量（0-1）
     * @param b 蓝色分量（0-1）
     * @param a 透明度分量（0-1）
     */
    virtual void setClearColor(float r, float g, float b, float a) = 0;

    /**
     * @brief 清除缓冲区
     * 
     * @param flags 清除标志位（颜色缓冲、深度缓冲、模板缓冲）
     */
    virtual void clear(uint32_t flags) = 0;

    /**
     * @brief 启用/禁用深度测试
     * 
     * @param enable 是否启用
     */
    virtual void enableDepthTest(bool enable) = 0;

    /**
     * @brief 启用/禁用混合
     * 
     * @param enable 是否启用
     */
    virtual void enableBlend(bool enable) = 0;
    /// @}

    /**
     * @brief 调整渲染目标尺寸
     * 
     * @param width 新的宽度
     * @param height 新的高度
     */
    virtual void resize(uint32_t width, uint32_t height) = 0;

    /**
     * @brief 获取GPU内存使用量
     * 
     * @return GPU内存使用量（字节）
     */
    virtual uint64_t getGPUMemoryUsage() const = 0;

    /**
     * @brief 获取原生渲染上下文
     * 
     * @return 原生上下文指针（如OpenGL的HGLRC）
     */
    virtual void*    getNativeContext() = 0;
};

/**
 * @brief 创建OpenGL渲染设备实例
 *
 * @return OpenGL设备实例指针，失败返回nullptr
 */
IDevice* createGLDevice();

/**
 * @brief 创建Null渲染设备实例（无GPU操作，用于测试）
 *
 * NullDevice 不执行任何实际的 GPU 操作，所有方法都是空实现或返回默认值。
 * 主要用于单元测试、CI/CD 自动化测试和后端抽象层验证。
 *
 * @return Null设备实例指针，永远不会返回nullptr
 */
IDevice* createNullDevice();

/**
 * @brief 创建Vulkan渲染设备实例（Phase 3 新增）
 *
 * Vulkan 后端提供跨平台 GPU 加速渲染，支持 Windows/Linux/macOS。
 * 需要安装 Vulkan SDK 才能启用编译。
 *
 * @return Vulkan设备实例指针，失败返回nullptr
 */
IDevice* createVulkanDevice();

/**
 * @brief 创建Metal渲染设备实例（Phase 3 新增）
 *
 * Metal 后端提供 macOS/iOS 平台的原生 GPU 渲染。
 * 需要 Apple 平台和 Metal 框架支持。
 *
 * @return Metal设备实例指针，失败返回nullptr
 */
IDevice* createMetalDevice();

}
