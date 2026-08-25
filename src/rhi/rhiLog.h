/**
 * @file rhiLog.h
 * @brief RHI 内部日志转发（只依赖注入的回调，不依赖任何日志库）
 *
 * 旧实现里 17 个文件硬依赖 `Log/SyLogger.h`，这让「渲染 DLL 零业务耦合」
 * 无法成立：宿主必须同时携带 SanYi 的 Log 库才能加载 RenderX。
 * 新 RHI 统一通过 DeviceDesc::logCallback 注入日志出口，
 * 未注入时静默丢弃（不是 fallback 到 stdout——库不该擅自污染宿主的输出）。
 */
#pragma once

#include "rhiGpuDevice.h"

#include <cstdarg>
#include <cstdio>

namespace Render::RHI
{

    /// 轻量日志转发器，按值持有回调，可安全拷贝进各资源对象
    class RhiLogger
    {
    public:
        RhiLogger() = default;
        RhiLogger(LogCallback callback, void* userData) : m_callback(callback), m_userData(userData) {}

        bool enabled() const { return m_callback != nullptr; }

        void write(LogLevel level, const char* format, ...) const
        {
            if (!m_callback || !format)
            {
                return;
            }
            char buffer[1024];
            va_list args;
            va_start(args, format);
            const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            if (written < 0)
            {
                return;
            }
            m_callback(level, buffer, m_userData);
        }

        void debug(const char* format, ...) const
        {
            if (!m_callback) { return; }
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            m_callback(LogLevel::Debug, buffer, m_userData);
        }

        void info(const char* format, ...) const
        {
            if (!m_callback) { return; }
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            m_callback(LogLevel::Info, buffer, m_userData);
        }

        void warn(const char* format, ...) const
        {
            if (!m_callback) { return; }
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            m_callback(LogLevel::Warn, buffer, m_userData);
        }

        void error(const char* format, ...) const
        {
            if (!m_callback) { return; }
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            m_callback(LogLevel::Error, buffer, m_userData);
        }

    private:
        LogCallback m_callback = nullptr;
        void* m_userData = nullptr;
    };

}  // namespace Render::RHI
