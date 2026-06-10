#include "app/levels/Level.h"

#include "app/scene/Scene.h"

void Level::Unload(const LevelLoadContext& ctx)
{
    ctx.scene.Clear();
}
