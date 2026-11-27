#include "app/scene/OceanWaterSurface.h"

#include "ocean/OceanReadback.h"
#include "ocean/OceanRenderable.h"

bool OceanWaterSurface::Present() const { return ocean_ && ocean_->IsVisibleGame(); }
bool OceanWaterSurface::HasWaves() const { return Present() && ocean_->GetReadback().HasSurface(); }
float OceanWaterSurface::Level() const { return ocean_ ? ocean_->GetWaterLevel() : 0.0f; }
float OceanWaterSurface::SampleHeight(float x, float z, float wakeShare) const
{
    return ocean_->GetReadback().SampleHeight(x, z, wakeShare);
}
