/**
 * @file render_runtime.h
 * @brief RenderX 运行时（进程级共享资源）
 *
 * RenderRuntime 持有跨会话共享的资源：
 * - shader 源码缓存（取代 shaders.cpp 的全局静态变量）
 * - 全局能力查询（backend capability）
 * - 共享资源池（font atlas、pipeline cache 等）
 *
 * 该结构体的生命周期应大于等于所有 RenderDevice/RenderSession。
 * 在当前阶段，它作为“会话级状态”和“共享级资源”的分界层。
 *
 * M1 任务：建立 RenderRuntime，逐步将 RenderDevice 中的共享资源
 * 迁移到这里，最终实现多窗口共享运行时、单窗口独享会话状态的架构。
 */
#pragma once

#include "shader/shaders.h"
#include "render/render_types.h"  // BackendType 定义

#include <cstdint>
#include <memory>
#include <mutex>

namespace render
{

    /**
     * @brief RenderX 运行时
     *
     * 进程级单例，管理跨会话共享的资源。
     */
    struct RenderRuntime
    {
        /**
         * @brief 创建或获取全局 RenderRuntime 实例
         *
         * @return RenderRuntime 引用（线程安全）
         */
        static RenderRuntime& instance();

        /**
         * @brief 初始化运行时（加载 shader、设置全局能力）
         *
         * @param shaderDir shader 文件目录路径
         * @param backend 当前使用的后端类型
         * @return 初始化是否成功
         */
        bool initialize(const std::string& shaderDir, render::BackendType backend);

        /**
         * @brief 关闭运行时，释放共享资源
         */
        void shutdown();

        /**
         * @brief 是否已初始化
         */
        bool isInitialized() const
        {
            return m_initialized;
        }

        /**
         * @brief 获取当前后端类型
         */
        render::BackendType getBackend() const
        {
            return m_backend;
        }

        /**
         * @brief Shader 源码管理
         *
         * 当前将 shader 源码缓存在 RenderRuntime 中，
         * 取代 shader::initialize() 的全局静态变量模式。
         */
        std::string shaderDir;                                        ///< shader 文件目录
        render::BackendType m_backend = render::BackendType::OpenGL;  ///< 当前后端
        bool m_initialized = false;

        // M1: RenderRuntime 使用 singleton + std::unique_ptr，需要公有析构
        // 防止外部代码误用，仅通过 instance() 获取
        ~RenderRuntime() = default;

    private:
        RenderRuntime() = default;
        RenderRuntime(const RenderRuntime&) = delete;
        RenderRuntime& operator=(const RenderRuntime&) = delete;

        mutable std::mutex m_mutex;  ///< 线程安全
    };

}  // namespace render
