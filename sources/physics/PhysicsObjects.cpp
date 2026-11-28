#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "physics/PhysicsInternal.h" // Register, Unregister, NotifyChanged: the worlds' queue
#include "rendering/renderables/ObjectWorldLinks.h"
#include "rendering/renderables/RenderableObject.h"

// Architecture plan R7.2: what the physics keeps per object (PhysicsWorld.h), in its table and not on the object.
namespace physics
{
    namespace
    {
        struct ObjectEntry
        {
            Motion motion = Motion::None;
            CollisionSettings collision;
            std::shared_ptr<const VesselSettings> vessel;
            std::uint32_t revision = 0; // moves with the collision and the vessel's numbers
            bool buoyant = false;
            bool registered = false;    // the worlds know it: its first motion
        };

        // Never destroyed: objects living in statics are torn down after function-local statics made later, and a link's
        // ObjectDetached from a destructor must not find the table gone.
        struct ObjectTable
        {
            std::mutex mutex;
            std::unordered_map<const RenderableObjectBase*, ObjectEntry> entries;
            std::vector<RenderableObject*> floaters; // the buoyant ones, in the order they became so
        };
        ObjectTable& TheTable()
        {
            static ObjectTable* table = new ObjectTable();
            return *table;
        }

        void DropFloater(ObjectTable& table, const RenderableObjectBase* object)
        {
            table.floaters.erase(std::remove_if(table.floaters.begin(), table.floaters.end(),
                                                [object](const RenderableObject* f) { return static_cast<const RenderableObjectBase*>(f) == object; }),
                                 table.floaters.end());
        }

        bool Registered(const RenderableObjectBase& object)
        {
            ObjectTable& table = TheTable();
            std::lock_guard<std::mutex> lock(table.mutex);
            const auto it = table.entries.find(&object);
            return it != table.entries.end() && it->second.registered;
        }

        // The object moved, was hidden or shown (a hidden object has no body): the worlds look at it again. It leaves the world
        // (RenderableObjectBase::DetachFromWorld, or its destructor): out of the table and the worlds -- a key only.
        class PhysicsObjectLink final : public ObjectWorldLink
        {
        public:
            const char* Name() const override { return "physics"; }
            void ObjectChanged(RenderableObjectBase& object) override
            {
                if (Registered(object)) { NotifyChanged(&object); }
            }
            void ObjectDetached(RenderableObjectBase& object) override
            {
                bool registered = false;
                {
                    ObjectTable& table = TheTable();
                    std::lock_guard<std::mutex> lock(table.mutex);
                    const auto it = table.entries.find(&object);
                    if (it != table.entries.end())
                    {
                        registered = it->second.registered;
                        table.entries.erase(it);
                    }
                    DropFloater(table, &object);
                }
                if (registered) { Unregister(&object); }
            }
        };

        unsigned LinkBit()
        {
            static PhysicsObjectLink link;
            static const unsigned bit = worldlinks::Add(link);
            return bit;
        }

        // A write to the object's entry (made at its first write, and the object linked): what `write` returns says whether the
        // worlds must hear of it, and they do if they know the object.
        template <typename Write>
        void Change(RenderableObject& object, Write&& write)
        {
            const unsigned bit = LinkBit();
            bool tell = false;
            {
                ObjectTable& table = TheTable();
                std::lock_guard<std::mutex> lock(table.mutex);
                ObjectEntry& entry = table.entries[&object];
                tell = write(entry) && entry.registered;
            }
            object.LinkToWorld(bit, true);
            if (tell) { NotifyChanged(&object); }
        }
    } // namespace

    void SetMotion(RenderableObject& object, Motion motion)
    {
        const unsigned bit = LinkBit();
        bool first = false;
        {
            ObjectTable& table = TheTable();
            std::lock_guard<std::mutex> lock(table.mutex);
            ObjectEntry& entry = table.entries[&object];
            entry.motion = motion;
            first = !entry.registered;
            entry.registered = true;
        }
        object.LinkToWorld(bit, true);
        if (first) { Register(&object); }
        else { NotifyChanged(&object); }
    }

    Motion MotionOf(const RenderableObject& object)
    {
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto it = table.entries.find(&object);
        return it != table.entries.end() ? it->second.motion : Motion::None;
    }

    void SetCollision(RenderableObject& object, CollisionSettings settings)
    {
        Change(object, [&settings](ObjectEntry& entry)
        {
            entry.collision = std::move(settings);
            ++entry.revision;
            return true;
        });
    }

    CollisionSettings CollisionOf(const RenderableObject& object)
    {
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto it = table.entries.find(&object);
        return it != table.entries.end() ? it->second.collision : CollisionSettings{};
    }

    void SetVessel(RenderableObject& object, std::shared_ptr<const VesselSettings> settings)
    {
        Change(object, [&settings](ObjectEntry& entry)
        {
            entry.vessel = std::move(settings);
            ++entry.revision;
            return true;
        });
    }

    std::shared_ptr<const VesselSettings> VesselOf(const RenderableObject& object)
    {
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto it = table.entries.find(&object);
        return it != table.entries.end() ? it->second.vessel : nullptr;
    }

    std::uint32_t BodyRevisionOf(const RenderableObject& object)
    {
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto it = table.entries.find(&object);
        return it != table.entries.end() ? it->second.revision : 0u;
    }

    void SetBuoyant(RenderableObject& object, bool buoyant)
    {
        {
            ObjectTable& table = TheTable();
            std::lock_guard<std::mutex> lock(table.mutex);
            const auto it = table.entries.find(&object);
            if ((it != table.entries.end() && it->second.buoyant) == buoyant) { return; }
        }
        Change(object, [&](ObjectEntry& entry)
        {
            entry.buoyant = buoyant;
            ObjectTable& table = TheTable(); // under Change's lock
            if (buoyant) { table.floaters.push_back(&object); }
            else { DropFloater(table, &object); }
            return true; // kinematic against static
        });
        if (!buoyant) { object.ClearRenderOffset(); }
    }

    bool IsBuoyant(const RenderableObject& object)
    {
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        const auto it = table.entries.find(&object);
        return it != table.entries.end() && it->second.buoyant;
    }

    void CollectFloaters(std::vector<Floater>& out)
    {
        out.clear();
        ObjectTable& table = TheTable();
        std::lock_guard<std::mutex> lock(table.mutex);
        for (RenderableObject* object : table.floaters)
        {
            const auto it = table.entries.find(object);
            out.push_back(Floater{ object, it != table.entries.end() && it->second.motion == Motion::Dynamic });
        }
    }
} // namespace physics
