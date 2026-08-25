/**
 * @file StubRHIFactories.cpp
 * @brief Stub RHI device factories for the Null-backend test executable.
 *
 * Runtime::create() references RHI::createGLDevice() / RHI::createVulkanDevice()
 * in its backend switch. The Null test only links rhinull.cpp (not rhigl.cpp),
 * so we provide no-op stubs here. They are never called by the Null tests.
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

        IDevice* createVulkanDevice()
        {
            return nullptr;
        }

    }  // namespace RHI
}  // namespace Render
