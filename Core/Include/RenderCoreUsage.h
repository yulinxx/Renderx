#pragma once

/**
 * @file RenderCoreUsage.h
 * @brief 新渲染架构使用示例
 *
 * 本文件展示如何使用新的RenderCore架构实现高效渲染
 */

#include "RenderCore.h"

namespace RenderCore
{

// ==================== 使用示例1: 基本用法 ========== =========

/**
 * @example 基本渲染流程
 *
 * // 1. 创建视图管理器（通常作为单例）
 * ViewManager viewManager;
 * viewManager.initialize();
 *
 * // 2. 创建渲染Widget并关联到视图管理器
 * RenderWidgetEx* widget = new RenderWidgetEx(parent, &viewManager);
 *
 * // 3. 添加实体（使用EntityId进行增量更新）
 * std::vector<Vertex> lineVertices = {
 *     {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // 红色
 *     {{100.0f, 100.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
 * };
 * widget->setEntity(1001, lineVertices, EPrimitiveType::Lines);
 *
 * // 4. 更新实体（无需重新上传所有数据）
 * std::vector<Vertex> newVertices = {
 *     {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // 绿色
 *     {{200.0f, 200.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
 * };
 * widget->setEntity(1001, newVertices, EPrimitiveType::Lines);  // ID相同，只更新数据
 *
 * // 5. 删除实体
 * widget->removeEntity(1001);
 */

// ==================== 使用示例2: 批量操作 ========== =========

/**
 * @example 批量添加实体
 *
 * // 准备数据
 * std::vector<EntityId> ids = {1, 2, 3, 4, 5};
 * std::vector<std::vector<Vertex>> verticesList = {
 *     line1, line2, line3, line4, line5
 * };
 * std::vector<EPrimitiveType> types(ids.size(), EPrimitiveType::Lines);
 * std::vector<float> widths(ids.size(), 1.0f);
 *
 * // 批量设置
 * widget->setEntities(ids, verticesList, types, widths);
 */

// ==================== 使用示例3: 多窗口共享数据 ========== =========

/**
 * @example 多窗口共享同一个World
 *
 * // 创建共享的ViewManager
 * ViewManager sharedViewManager;
 * sharedViewManager.initialize();
 *
 * // 创建多个视图窗口
 * RenderWidgetEx* view1 = new RenderWidgetEx(parent1, &sharedViewManager);
 * RenderWidgetEx* view2 = new RenderWidgetEx(parent2, &sharedViewManager);
 *
 * // 在任一窗口中添加数据，两边都会显示
 * sharedViewManager.getWorld()->setEntity(1001, vertices, EPrimitiveType::Lines);
 *
 * // 任一窗口请求更新，所有窗口同步刷新
 * view1->requestUpdate();
 */

// ==================== 使用示例4: 自定义渲染回调 ========== =========

/**
 * @example 自定义渲染回调
 *
 * viewManager.setPreRenderCallback([](IRenderView* view) {
 *     // 渲染前回调 - 可以设置额外的OpenGL状态
 *     glEnable(GL_DEPTH_TEST);
 * });
 *
 * viewManager.setPostRenderCallback([](IRenderView* view) {
 *     // 渲染后回调 - 可以恢复状态或绘制额外内容
 *     glDisable(GL_DEPTH_TEST);
 * });
 */

// ==================== 架构说明 ========== =========

/**
 * @page architecture 新渲染架构设计
 *
 * @section dataflow 数据流
 *
 * @code
 * [Engine层] --> setEntity() --> [RenderWorld] --> update() --> [GL46Backend]
 *                                                            |
 *                                                            v
 *                                                       [BatchRenderer]
 *                                                            |
 *                                                            v
 *                                                        [GPU绘制]
 * @endcode
 *
 * @section incremental 增量更新机制
 *
 * 每个Entity有唯一的ID和Generation：
 * - Add: 新Entity，第一次上传
 * - Modify: 如果Generation变化，才上传顶点数据
 * - Remove: 软删除，标记为deleted
 *
 * @section batch 批量渲染
 *
 * 相同图元类型的Entity会被合并为单个批量绘制命令，
 * 使用glMultiDrawArraysIndirect减少Draw Call。
 *
 * @section future 未来扩展
 *
 * - Vulkan后端: 实现IVulkanBackend接口
 * - LOD系统: 根据视锥剔除结果动态调整细分级别
 * - 多线程: Tessellation和渲染并行化
 */

} // namespace RenderCore
