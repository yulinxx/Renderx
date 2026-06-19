#pragma once

/**
 * @file RenderCore.h
 * @brief 高性能渲染核心 - 主头文件
 *
 * 新架构特点：
 * 1. 面向数据的设计(DOD) - 连续内存布局
 * 2. 增量更新 - 利用Entity ID实现增删改
 * 3. 批量渲染 - glMultiDrawArraysIndirect
 * 4. 多后端支持 - OpenGL 4.6, Vulkan-ready
 * 5. 多窗口共享 - ViewManager管理
 */

// 核心接口
#include "RenderCore/IRenderBackend.h"
#include "RenderCore/RenderWorld.h"
#include "RenderCore/RenderBuffer.h"
#include "RenderCore/RenderBatch.h"
#include "RenderCore/RenderView.h"

// Qt集成
#include "RenderCore/RenderWidgetEx.h"

// 版本信息
#define RENDER_CORE_VERSION_MAJOR 1
#define RENDER_CORE_VERSION_MINOR 0
#define RENDER_CORE_VERSION_PATCH 0

namespace RenderCore
{
    // 版本信息
    constexpr int getVersionMajor() { return RENDER_CORE_VERSION_MAJOR; }
    constexpr int getVersionMinor() { return RENDER_CORE_VERSION_MINOR; }
    constexpr int getVersionPatch() { return RENDER_CORE_VERSION_PATCH; }
}
