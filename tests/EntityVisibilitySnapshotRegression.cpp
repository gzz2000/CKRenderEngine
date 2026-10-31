#include "RCK2dEntity.h"
#include "RCK3dEntity.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "CKEntityVisibilityState.h"
#include <cstdio>
#include <cstdlib>

extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

int main() {
    SetProcessorSpecific_FunctionsPtr();
    Require(CKStartUp() == CK_OK, "startup");
    CKCLASSREGISTERCID(RCKRenderObject, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCK2dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCK3dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCKRenderContext, CKCID_OBJECT);
    CKBuildClassHierarchyTable();
    CKContext *context = nullptr;
    Require(CKCreateContext(&context, nullptr, 0, 0) == CK_OK, "context");
    new RCKRenderManager(context);
    auto *entity = static_cast<RCK3dEntity *>(context->CreateObject(CKCID_3DENTITY, "history"));
    Require(entity && entity->m_SceneGraphNode, "entity node");
    auto *node = entity->m_SceneGraphNode;
    CKEntityVisibilityState saved;
    for (unsigned history = 0; history < 4; ++history) {
        node->m_Flags = history | CKSGN_NEEDSORT;
        Require(CKCaptureEntityVisibilityState(entity, &saved, sizeof(saved)), "overwrite capture");
        Require(saved.frustumFlags == history && saved.hasSceneGraphNode == 1, "capture historical bits only");
        Require(node->m_Flags == (history | CKSGN_NEEDSORT), "capture has no traversal side effect");
        for (unsigned future = 0; future < 4; ++future) {
            node->m_Flags = future | CKSGN_BOX_MASK | CKSGN_INTRANSPARENTLIST;
            Require(bool(entity->IsAllOutsideFrustrum()) == bool(future & CKSGN_OUTSIDEFRUSTUM), "abandoned timeline affects shadow floor query");
            for (int repeat = 0; repeat < 8; ++repeat) {
                Require(CKRestoreEntityVisibilityState(entity, &saved, sizeof(saved)), "repeat restore");
                Require(bool(entity->IsAllOutsideFrustrum()) == bool(history & CKSGN_OUTSIDEFRUSTUM), "shadow floor query observes saved history");
                Require(node->m_Flags == (history | CKSGN_BOX_MASK | CKSGN_INTRANSPARENTLIST), "derived box/list cache flags remain independent");
            }
        }
    }
    const auto before = node->m_Flags;
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto bad = saved;
        if (invalid == 0) ++bad.version;
        if (invalid == 1) ++bad.object;
        if (invalid == 2) bad.hasSceneGraphNode = 2;
        if (invalid == 3) bad.frustumFlags |= CKSGN_BOXVALID;
        if (invalid == 4) bad.hasSceneGraphNode = 0;
        Require(!CKRestoreEntityVisibilityState(entity, &bad, sizeof(bad)), "invalid state rejected");
        Require(node->m_Flags == before, "rejection is nonmutating");
    }
    Require(!CKRestoreEntityVisibilityState(entity, &saved, sizeof(saved) - 1), "reject wrong ABI size");
    entity->m_SceneGraphNode = nullptr;
    Require(!CKRestoreEntityVisibilityState(entity, &saved, sizeof(saved)), "reject disappeared node");
    CKEntityVisibilityState absent;
    Require(CKCaptureEntityVisibilityState(entity, &absent, sizeof(absent)), "capture absent node");
    Require(!absent.hasSceneGraphNode && !absent.frustumFlags, "absent node has no history");
    Require(CKRestoreEntityVisibilityState(entity, &absent, sizeof(absent)), "restore absent node");
    entity->m_SceneGraphNode = node;
    Require(!CKRestoreEntityVisibilityState(entity, &absent, sizeof(absent)), "reject new node");
    Require(node->m_Flags == before, "node rejection is nonmutating");
    Require(CKCloseContext(context) == CK_OK, "close");
    Require(CKShutdown() == CK_OK, "shutdown");
    std::printf("PASS %u visibility history checks\n", checks);
}
