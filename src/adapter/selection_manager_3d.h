#pragma once

#include <vector>
#include "render/render.h"

namespace Eg {

class SyMeshEntity;

class RENDER_API SelectionManager3D
{
public:
    void selectAll() {}
    void clearSelection() {}
    bool hasSelection() const { return false; }
    const std::vector<SyMeshEntity*>& getSelectedEntities() const { return m_empty; }

private:
    std::vector<SyMeshEntity*> m_empty;
};

}
