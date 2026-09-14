// ============================================================================
// rx_push_constants.metal —— MSL 侧的 pushConstant 块声明（唯一声明处）
//
// 必须与 rx_push_constants.glsl 的 std140 块、以及 C++ 侧
// Render::RT::detail::PushConstants 逐字节一致（sizeof == 128）。
// 由 EmbedShaders.cmake 在构建期展开到各 .metal 文件。
//
// 注意：本文件内不得出现「包含指令」的字面写法，连注释里也不行——
// 展开器是纯文本替换，会把注释里的写法当成真的指令，从而循环展开。
//
// std140 与 MSL 的差异只有一处，但很致命：
//   std140 的 vec3 占 12 字节，紧随的 float 落在偏移 +12；
//   MSL 的 float3 是 16 字节（同 simd 类型），紧随字段会被推到 +16。
// 直接照抄会让后半段整体错位且**不会有任何编译错误**——症状是材质参数
// 读到邻字段的值。因此这里用 float4 承载「vec3 + 紧随的标量」。
//
// std140 偏移：
//   uView         0..63
//   uViewport     64..71
//   uPointSize    72..75
//   uPad0         76..79
//   uMatDiffuse   80..95
//   uMatAmbient   96..111   （vec3 96..107 + pad1 108..111）
//   uMatSpecular  112..127  （vec3 112..123 + shininess 124..127）
// ============================================================================

#ifndef RX_PUSH_CONSTANTS_METAL
#define RX_PUSH_CONSTANTS_METAL

struct RxPushConstants
{
    float4x4 uView;
    float2 uViewport;
    float uPointSize;
    float uPad0;
    float4 uMatDiffuse;
    // xyz = 环境色系数，w = 占位（对应 std140 的 pad1）
    float4 uMatAmbient;
    // xyz = 高光颜色，w = 高光指数（对应 std140 的 uMatShininess）
    float4 uMatSpecular;
};

#endif
