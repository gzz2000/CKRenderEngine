#include "CKEntityUserBoxState.h"
#include "RCK3dEntity.h"

#if defined(_WIN32)
#define CK_USERBOX_EXPORT __declspec(dllexport)
#else
#define CK_USERBOX_EXPORT __attribute__((visibility("default")))
#endif

extern "C" CK_USERBOX_EXPORT int CKCaptureEntityUserBoxState(
    CKObject *object, CKEntityUserBoxState *state, std::size_t bytes) {
    if (!object || !state || bytes != sizeof(*state) ||
        !CKIsChildClassOf(object->GetClassID(), CKCID_3DENTITY)) return 0;
    auto *entity = static_cast<RCK3dEntity *>(object);
    *state = CKEntityUserBoxState{};
    state->object = entity->GetID();
    state->enabled = (entity->m_MoveableFlags & VX_MOVEABLE_USERBOX) != 0;
    if (state->enabled) state->local = entity->m_LocalBoundingBox;
    return 1;
}

extern "C" CK_USERBOX_EXPORT int CKRestoreEntityUserBoxState(
    CKObject *object, const CKEntityUserBoxState *state, std::size_t bytes) {
    if (!object || !state || bytes != sizeof(*state) || state->version != 1 ||
        state->enabled > 1 || object->GetID() != state->object ||
        !CKIsChildClassOf(object->GetClassID(), CKCID_3DENTITY)) return 0;
    auto *entity = static_cast<RCK3dEntity *>(object);
    entity->m_MoveableFlags &= ~(VX_MOVEABLE_USERBOX | VX_MOVEABLE_UPTODATE |
        VX_MOVEABLE_BOXVALID | VX_MOVEABLE_EXTENTSUPTODATE);
    if (state->enabled) {
        entity->m_LocalBoundingBox = state->local;
        // IsInViewFrustrum uses a user box only while BOXVALID is set.
        // Leaving it invalid does not trigger a lazy rebuild there: a
        // meshless particle emitter falls back to its origin and can vanish
        // from the transparent list until its behavior sets another box.
        // Rebuild the derived world box exactly as SetBoundingBox(local) does,
        // so culling can immediately consume the restored authored box.
        entity->m_WorldBoundingBox.TransformFrom(state->local, entity->m_WorldMatrix);
        entity->m_MoveableFlags |= VX_MOVEABLE_USERBOX | VX_MOVEABLE_BOXVALID;
    }
    if (entity->m_SceneGraphNode) entity->m_SceneGraphNode->InvalidateBox(TRUE);
    return 1;
}
#undef CK_USERBOX_EXPORT
