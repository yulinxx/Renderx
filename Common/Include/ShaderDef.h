#pragma once

/**
 * @brief 存储所有的 OpenGL 着色器源码字符串
 */
namespace Shaders
{
    // 基础线段着色器
    extern const char* LINE_VS;
    extern const char* LINE_FS;

    // 十字光标色器
    extern const char* CROSS_VS;
    extern const char* CROSS_FS;

    // 标尺着色器
    extern const char* RULER_VS;
    extern const char* RULER_FS;

    // 贝塞尔曲线着色器
    extern const char* BEZIER_VS;
    extern const char* BEZIER_FS;

    // 控制线着色器
    extern const char* CONTROL_VS;
    extern const char* CONTROL_FS;

    // 位图贴图着色器
    extern const char* BITMAP_VS;
    extern const char* BITMAP_FS;

    // 捕捉标记着色器
    extern const char* SNAP_VS;
    extern const char* SNAP_FS;

    // 屏幕空间固定大小的方形点标记着色器（用于选择手柄）
    extern const char* POINT_MARKER_VS;
    extern const char* POINT_MARKER_FS;

    // 3D 网格着色器 (Phong 光照)
    extern const char* MESH3D_VS;
    extern const char* MESH3D_FS;

    // 3D 网格/参考平面着色器
    extern const char* GRID3D_VS;
    extern const char* GRID3D_FS;

    // 3D 边界框着色器
    extern const char* BBOX3D_VS;
    extern const char* BBOX3D_FS;

    // 3D 选中高亮着色器 (边缘发光)
    extern const char* HIGHLIGHT3D_VS;
    extern const char* HIGHLIGHT3D_FS;
}
