#pragma once
// Architecture plan R7.2 (docs/architecture_review_plan.md): what the modules above rendering keep per object -- the
// ocean's water mask and buoyancy, the physics world -- they keep in tables of their own, and the object tells them
// through this list what they must hear of it: a move or a change of visibility (Changed), its leaving the world
// (Detached). The object holds only a mask of the links that know it (RenderableObjectBase::WorldLinks), so an object no
// module knows pays one test; it names no module: RenderableObject.h includes nothing of ocean/ or physics/.
#include <cstdint>

class RenderableObjectBase;

class ObjectWorldLink
{
public:
    virtual const char* Name() const = 0;
    // The object's transform or visibility changed (the game half).
    virtual void ObjectChanged(RenderableObjectBase& /*object*/) {}
    // The object leaves the world (RenderableObjectBase::DetachFromWorld, or its destructor without it): every entry of it
    // goes. The object is still whole at the first, being destroyed at the second: use it as a key only.
    virtual void ObjectDetached(RenderableObjectBase& object) = 0;

protected:
    ~ObjectWorldLink() = default; // a module's static: never deleted through this type
};

namespace worldlinks
{
// A module's link, once (a function-local static of the module's): its bit in the objects' masks, 0..7.
unsigned Add(ObjectWorldLink& link);
// Tell the links in `links` (an object's mask) of the object.
void Changed(RenderableObjectBase& object, std::uint8_t links);
void Detached(RenderableObjectBase& object, std::uint8_t links);
} // namespace worldlinks
