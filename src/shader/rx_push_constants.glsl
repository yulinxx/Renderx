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
// （src/rt/rxInternal.h，有 static_assert(sizeof == 80) 锁定）。
// std140 布局不匹配不会产生任何编译错误，只会算出错误的偏移——
// 表现为「矩阵读到一半是视口尺寸」这类无从下手的画面错乱，
// 因此绝不能在各 shader 里各抄一份。
//
// std140 偏移：
//   uView      0..63   （mat4，16 字节对齐）
//   uViewport  64..71  （vec2，8 字节对齐）
//   uPointSize 72..75
//   uSdfScale  76..79
// 合计 80 字节，在 RHI 的 kMaxPushConstantBytes = 128 之内。
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
    /// SDF 字形的边缘锐度比例
    float uSdfScale;
};
