#include "CKEntityVisibilityState.h"
#include "RCK3dEntity.h"

#if defined(_WIN32)
#define CK_VISIBILITY_EXPORT __declspec(dllexport)
#else
#define CK_VISIBILITY_EXPORT __attribute__((visibility("default")))
#endif

extern "C" CK_VISIBILITY_EXPORT int CKCaptureEntityVisibilityState(
    CKObject *object, CKEntityVisibilityState *state, std::size_t bytes) {
    if (!object || !state || bytes != sizeof(*state) ||
        !CKIsChildClassOf(object->GetClassID(), CKCID_3DENTITY)) return 0;
    auto *entity = static_cast<RCK3dEntity *>(object);
    *state = CKEntityVisibilityState{};
    state->object = entity->GetID();
    state->hasSceneGraphNode = entity->m_SceneGraphNode != nullptr;
    if (entity->m_SceneGraphNode)
        state->frustumFlags = entity->m_SceneGraphNode->GetFlags() & CKSGN_FRUSTUM_MASK;
    return 1;
}

extern "C" CK_VISIBILITY_EXPORT int CKRestoreEntityVisibilityState(
    CKObject *object, const CKEntityVisibilityState *state, std::size_t bytes) {
    if (!object || !state || bytes != sizeof(*state) || state->version != 1 ||
        state->object != object->GetID() || state->hasSceneGraphNode > 1 ||
        (state->frustumFlags & ~CKDWORD(CKSGN_FRUSTUM_MASK)) ||
        (!state->hasSceneGraphNode && state->frustumFlags) ||
        !CKIsChildClassOf(object->GetClassID(), CKCID_3DENTITY)) return 0;
    auto *entity = static_cast<RCK3dEntity *>(object);
    auto *node = entity->m_SceneGraphNode;
    if ((node != nullptr) != (state->hasSceneGraphNode != 0)) return 0;
    if (node) {
        // Change only the saved historical query result. No traversal, culling,
        // hierarchy update, parent propagation or bounding-box recomputation.
        node->ClearFlags(CKSGN_FRUSTUM_MASK);
        node->SetFlags(state->frustumFlags);
    }
    return 1;
}
#undef CK_VISIBILITY_EXPORT
