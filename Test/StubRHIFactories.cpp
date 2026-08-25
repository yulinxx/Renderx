/**
 * @file StubRHIFactories.cpp
 * @brief Null 后端测试可执行文件所需的 RHI 工厂桩实现。
 *
 * Runtime::create() 的后端 switch 引用了 RHI::createGLDevice()，
 * 而 Null 测试目标只链接 rhiNull.cpp（不链接 rhiGl.cpp），
 * 因此在此提供一个空桩。Null 测试永远不会调用它。
 */
#include "rhi/rhiDevice.h"

namespace Render
{
    namespace RHI
    {

        IDevice* createGLDevice()
        {
            return nullptr;
        }

    }  // namespace RHI
}  // namespace Render
