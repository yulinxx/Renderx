// ============================================================================
// rx_push_constants.glsl —— RT 全部 shader 共用的 pushConstant 块
//
// **本文件是唯一声明处**，由 EmbedShaders.cmake 在构建期展开到各 shader
// （各 shader 在 #version 之后写一行 include 指令引入本文件）。
//
// 注意：本文件内不得出现 include 指令的字面写法，连注释里也不行——
// 展开器是纯文本替换，会把注释里的写法当成真的指令，从而循环展开。
//
// 字段顺序与类型必须与 C++ 侧 Render::RT::detail::PushConstants 逐字节一致
// （src/rt/rxInternal.h，有 static_assert(sizeof == 128) 与两条 offsetof
// 断言锁定）。std140 布局不匹配不会产生任何编译错误，只会算出错误的偏移——
// 表现为「矩阵读到一半是视口尺寸」这类无从下手的画面错乱，
// 因此绝不能在各 shader 里各抄一份。
//
// std140 偏移：
//   uView         0..63    （mat4，16 字节对齐）
//   uViewport     64..71   （vec2，8 字节对齐）
//   uPointSize    72..75
//   uPad0         76..79
//   uMatDiffuse   80..95   （vec4）
//   uMatAmbient   96..107  （vec3，16 字节对齐 → 后面必须补 4 字节）
//   uPad1         108..111
//   uMatSpecular  112..123 （vec3，同样 16 字节对齐）
//   uMatShininess 124..127
// 合计 128 字节，正好等于 RHI 的 kMaxPushConstantBytes。
//
// 为什么 2D shader 也声明后半段：块声明只有一处，尺寸就必须统一。
// 让 2D 只声明前 80 字节在 GL 上虽然合法，但等于把「同一块有两种长度」
// 这件事引入构建期，任何一次字段插入都会让两种声明的偏移悄悄错位。
// 后半段对 2D 只是未使用的 48 字节，代价可忽略。
//
// GL 后端把该块落地为绑定在 0 号 UBO binding point 的 uniform buffer
// （见 GlDevice::kPushConstantBinding / kPushConstantBlockName），
// Vulkan 映射为 push constant，Metal 映射为 setVertexBytes。
// 块名必须是 PushConstants：GL 后端按名字解析 block index。
// ============================================================================

layout(std140) uniform PushConstants
{
    /// 世界 → 裁剪空间矩阵，列主序。Screen 空间管线不使用此值。
    mat4 uView;
    /// 视口像素尺寸（宽, 高）。屏幕空间与 WorldPinned 需要它换算像素。
    vec2 uViewport;
    /// 点图元直径（像素），写入 gl_PointSize
    float uPointSize;
    /// 占位，让块尺寸与 C++ 侧一致（std140 把块尺寸向上取整到 16 的倍数）。
    /// 曾是 uSdfScale，随伪 SDF 文本路径一并作废，见 PushConstants::pad0。
    float uPad0;

    // ---- 3D 材质段（仅 P3N3 管线使用）----
    // 材质是 per-draw 的（同一帧不同网格各有颜色），因此必须在这个
    // 「每次换管线/材质都重推」的块里，而不能放到 per-pass 的 FrameUniforms。
    /// 漫反射基色（RGBA，A 参与混合）
    vec4 uMatDiffuse;
    /// 环境色系数（与 FrameUniforms 的环境光相乘）
    vec3 uMatAmbient;
    float uPad1;
    /// 高光颜色
    vec3 uMatSpecular;
    /// Blinn-Phong 高光指数。越大高光越锐。
    float uMatShininess;
};
