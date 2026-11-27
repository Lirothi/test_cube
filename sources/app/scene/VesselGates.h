#pragma once

#include <vector>

#include "physics/PhysicsWorld.h" // physics::ExtraGate

// The boats' gates (architecture plan R14.3a): what --physics-gate and intent_regression hand physics::RunGates beside the
// physics' own. "vessel": a boat on physics::VesselBench through the model's functions (app/scene/VesselModel.h); "helm":
// the helm's road through a world (app/scene/VesselSystem).
std::vector<physics::ExtraGate> VesselGates();
