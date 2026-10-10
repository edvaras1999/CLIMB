#pragma once

#include <cmath>

#include "Engine.h"
#include "Havok.h"
#include "skse/GameReferences.h"
#include "src/Core.h"

namespace fc {
namespace le {

constexpr bool climbableHit(unsigned layer, bool hasReference, unsigned formType) {
    if(!hasReference || formType == kFormType_NPC) return false;
    switch(layer) {
    case Havok::kLayer_NonCollidable:
    case Havok::kLayer_CharController:
    case Havok::kLayer_Weapon:
    case Havok::kLayer_Projectile:
    case Havok::kLayer_Transparent:
    case Havok::kLayer_Clutter:
    case Havok::kLayer_Biped:
    case Havok::kLayer_ActorZone:
    case Havok::kLayer_DebrisLarge:
        return false;
    default:
        return true;
    }
}

static_assert(!climbableHit(Havok::kLayer_Clutter, true, 0), "clutter must not be a traversal surface");
static_assert(!climbableHit(Havok::kLayer_Biped, true, 0), "actors must not be traversal surfaces");
static_assert(!climbableHit(Havok::kLayer_Static, false, 0), "unowned collision must not be a traversal surface");
static_assert(!climbableHit(Havok::kLayer_Static, true, kFormType_NPC), "NPCs must not be traversal surfaces");
static_assert(climbableHit(Havok::kLayer_Static, true, 0), "referenced static geometry must remain climbable");
static_assert(climbableHit(Havok::kLayer_Props, true, 0), "referenced props follow FreeRun's layer policy");

class TraversalWorld final : public fc::World {
public:
    explicit TraversalWorld(Actor * actor) : actor_(actor) {}

    std::optional<fc::Hit> ray(fc::Vec from, fc::Vec to) override {
        const fc::Vec delta = to - from;
        const float distance = delta.length();
        if(!actor_ || !from.finite() || !to.finite() || !std::isfinite(distance) || distance <= 0.001f)
            return {};

        const Havok::Result result = Havok::RayCast(
            NiPoint3(from.x, from.y, from.z),
            NiPoint3(delta.x, delta.y, delta.z),
            distance, Havok::kLayer_Transparent, actor_);
        // Keep the nearest hit even when it is not climbable; World::ray is also used for clearance.
        if(result.hitCount) {
            const Havok::Hit & hit = result.hits[0];
            return fc::Hit{
                fc::Vec{hit.position.x, hit.position.y, hit.position.z},
                fc::Vec{hit.normal.x, hit.normal.y, hit.normal.z},
                climbableHit(hit.layer, hit.hasReference, hit.formType)
            };
        }
        if(!result.didHit) return {};

        return fc::Hit{
            fc::Vec{result.hitPosition.x, result.hitPosition.y, result.hitPosition.z},
            fc::Vec{result.normal.x, result.normal.y, result.normal.z},
            climbableHit(result.layer, result.hasReference, result.formType)
        };
    }

private:
    Actor * actor_;
};

} // namespace le
} // namespace fc
