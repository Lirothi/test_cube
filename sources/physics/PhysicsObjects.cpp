#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "rendering/renderables/ObjectWorldLinks.h"
#include "rendering/renderables/RenderableObject.h"

// Architecture plan R7.2: what the physics keeps per object (PhysicsWorld.h), in its table and not on the object.
namespace physics
{
    namespace
    {
        struct ObjectEntry
        {
            bool buoyant = false;
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

        // The object leaves the world (RenderableObjectBase::DetachFromWorld, or its destructor): out of the table. A key only.
        class PhysicsObjectLink final : public ObjectWorldLink
        {
        public:
            const char* Name() const override { return "physics"; }
            void ObjectDetached(RenderableObjectBase& object) override
            {
                ObjectTable& table = TheTable();
                std::lock_guard<std::mutex> lock(table.mutex);
                table.entries.erase(&object);
                DropFloater(table, &object);
            }
        };

        unsigned LinkBit()
        {
            static PhysicsObjectLink link;
            static const unsigned bit = worldlinks::Add(link);
            return bit;
        }
    } // namespace

    void SetBuoyant(RenderableObject& object, bool buoyant)
    {
        const unsigned bit = LinkBit();
        {
            ObjectTable& table = TheTable();
            std::lock_guard<std::mutex> lock(table.mutex);
            const auto it = table.entries.find(&object);
            const bool was = it != table.entries.end() && it->second.buoyant;
            if (was == buoyant) { return; }
            table.entries[&object].buoyant = buoyant;
            if (buoyant) { table.floaters.push_back(&object); }
            else { DropFloater(table, &object); }
        }
        object.LinkToWorld(bit, true);
        if (!buoyant) { object.ClearRenderOffset(); }
        NotifyChanged(&object); // kinematic against static; a world that does not know the object lets it pass
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
        for (RenderableObject* object : table.floaters) { out.push_back(Floater{ object, object->GetPhysicsMotion() == Motion::Dynamic }); }
    }
} // namespace physics
