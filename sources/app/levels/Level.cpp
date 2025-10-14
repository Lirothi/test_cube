#include "app/levels/Level.h"

#include "app/Scene.h"
#include "app/Systems.h"

void Level::Unload(const LevelLoadContext& ctx)
{
    (void)ctx;
    Systems::GetScene().Clear();
}
