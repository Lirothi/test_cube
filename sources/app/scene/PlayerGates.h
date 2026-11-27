#pragma once

#include <vector>

#include "physics/PhysicsWorld.h" // physics::ExtraGate

// The player's gate (architecture plan R14.6): "player" of the world's layer, run after the physics' gate of that name.
// The physics' gate moves the capsule by hand; this one takes the game's road through the world's own frame
// (physics::World::Tick at 60 and at 300 frames a second): the player moves on the frame's clock, not the fixed step.
std::vector<physics::ExtraGate> PlayerGates();

// Every gate of the world's layer (app/scene) -- the boats' (VesselGates) and the player's -- as --physics-gate and
// intent_regression hand them to physics::RunGates.
std::vector<physics::ExtraGate> SceneGates();
