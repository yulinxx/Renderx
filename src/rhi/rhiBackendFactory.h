/**
 * @file rhiBackendFactory.h
 * @brief 后端设备的内部工厂声明
 *
 * 公共入口是 rhiGpuDevice.h 的 createDevice(DeviceDesc)。本文件只在
 * rhiFactory.cpp 与各后端实现之间共享，不对 RHI 之外可见。
 *
 * 之所以不把这些声明放进 rhiGpuDevice.h：旧版 rhiDevice.h 就是那么做的
 * （声明了 createVulkanDevice / createMetalDevice），结果两个后端被删除后
 * 声明仍留在头里，任何人都能写出链接期才失败的调用。
 */
#pragma once

#include "rhiGpuDevice.h"

namespace Render::RHI
{

    /// Null 后端：不触碰任何 GPU，用于单测与无头环境。始终可用。
    IGpuDevice* createNullDevice(const DeviceDesc& desc);

    /// OpenGL 后端。上下文由 ISurface 侧管理（当前为宿主注入的 ForeignGlContext）。
    IGpuDevice* createGlDevice(const DeviceDesc& desc);

#if defined(__APPLE__)
    /**
     * @brief Metal 后端（仅 Apple 平台编译）
     *
     * 非 Apple 平台没有这个符号：Metal 后端只在 APPLE 条件下纳入构建，
     * 声明也一并条件化，避免在别的平台上写出一个链接期才失败的调用。
     */
    IGpuDevice* createMetalDevice(const DeviceDesc& desc);
#endif

}  // namespace Render::RHI
