#include "CKAll.h"
#include "CKObjectManager.h"
#include <new>
#include "RCK3dEntity.h"
#include "RCKMesh.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "CKRenderCallbackSnapshot.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool ok, const char *name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
static int A(CKRenderContext *, CKRenderObject *, void *) { return 1; }
static int B(CKRenderContext *, CKRenderObject *, void *) { return 1; }
static void ContextCallback(CKRenderContext *, void *) {}
static void MeshCallback(CKRenderContext *, CK3dEntity *, CKMesh *, void *) {}
static std::vector<void *> QueueFunctions(const XClassArray<VxCallBack> &queue) {
    std::vector<void *> result;
    for (const auto &cb : queue) { result.push_back(cb.argument); result.push_back(cb.arg2); }
    return result;
}
int main() {
    SetProcessorSpecific_FunctionsPtr(); Require(CKStartUp() == CK_OK, "startup");
    CKCLASSREGISTERCID(RCKRenderObject, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCK3dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCKMesh, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCKRenderContext, CKCID_OBJECT);
    CKBuildClassHierarchyTable();
    CKContext *ctx = nullptr; Require(CKCreateContext(&ctx, nullptr, 0, 0) == CK_OK, "context");
    auto *manager = new RCKRenderManager(ctx);
    auto *entity = static_cast<RCK3dEntity *>(ctx->CreateObject(CKCID_3DENTITY, "callbacks"));
    auto *empty = static_cast<RCK3dEntity *>(ctx->CreateObject(CKCID_3DENTITY, "empty"));
    auto *mesh = static_cast<RCKMesh *>(ctx->CreateObject(CKCID_MESH, "mesh"));
    auto *view = static_cast<RCKRenderContext *>(ctx->CreateObject(CKCID_RENDERCONTEXT, "view"));
    Require(entity && empty && mesh && view, "registered owners");
    Require(CKIsChildClassOf(entity, CKCID_OBJECT), "complete object hierarchy");
    Require(ctx->GetObjectListByType(CKCID_OBJECT, TRUE).Size() >= 4, "enumerated owners");
    int a = 1, b = 2, c = 3;
    entity->AddPreRenderCallBack(A, &a, FALSE);
    entity->AddPostRenderCallBack(A, &b, TRUE);
    entity->AddPostRenderCallBack(B, &c, FALSE);
    entity->SetRenderCallBack(B, &a);
    mesh->AddPostRenderCallBack(MeshCallback, &a, TRUE);
    view->AddPreRenderCallBack(ContextCallback, &a, TRUE);
    view->AddPostRenderCallBack(ContextCallback, &b, TRUE, FALSE);
    view->AddPostSpriteRenderCallBack(ContextCallback, &c, FALSE);
    const auto expectedPreQueue = QueueFunctions(manager->m_TemporaryPreRenderCallbacks);
    const auto expectedPostQueue = QueueFunctions(manager->m_TemporaryPostRenderCallbacks);
    const auto *beforeContainer = entity->GetCallbackContainer();
    CKRenderCallbackSnapshot snapshot = nullptr;
    Require(CKCaptureRenderCallbacks(ctx, &snapshot) == CK_OK, "capture");
    Require(entity->GetCallbackContainer() == beforeContainer, "capture preserves allocation");
    Require(expectedPreQueue == QueueFunctions(manager->m_TemporaryPreRenderCallbacks) &&
            expectedPostQueue == QueueFunctions(manager->m_TemporaryPostRenderCallbacks), "capture preserves queues");
    for (int repeat = 0; repeat < 4; ++repeat) {
        entity->RemoveAllCallbacks();
        entity->AddPostRenderCallBack(B, &a, TRUE);
        empty->AddPostRenderCallBack(A, &a, TRUE);
        mesh->RemoveAllCallbacks();
        view->ClearCallbacks();
        Require(CKRestoreRenderCallbacks(ctx, snapshot) == CK_OK, "restore");
        const auto *callbacks = entity->GetCallbackContainer();
        Require(callbacks && callbacks->m_PreCallBacks.Size() == 1 && callbacks->m_PostCallBacks.Size() == 2, "counts");
        Require(callbacks->m_PreCallBacks[0].callback == reinterpret_cast<void *>(A) &&
                callbacks->m_PreCallBacks[0].argument == &a, "pre callback");
        Require(callbacks->m_PostCallBacks[0].callback == reinterpret_cast<void *>(A) &&
                callbacks->m_PostCallBacks[0].argument == &b && callbacks->m_PostCallBacks[0].temp &&
                callbacks->m_PostCallBacks[1].callback == reinterpret_cast<void *>(B) &&
                callbacks->m_PostCallBacks[1].argument == &c && !callbacks->m_PostCallBacks[1].temp, "post order and flags");
        Require(callbacks->m_Callback && callbacks->m_Callback->callback == reinterpret_cast<void *>(B) &&
                callbacks->m_Callback->argument == &a, "main callback");
        Require(!empty->GetCallbackContainer(), "restore previously absent container");
        Require(expectedPreQueue == QueueFunctions(manager->m_TemporaryPreRenderCallbacks) &&
                expectedPostQueue == QueueFunctions(manager->m_TemporaryPostRenderCallbacks), "temporary queue order");
        Require(mesh->RemovePostRenderCallBack(MeshCallback, &a), "mesh callback restored");
        // Removed temporary callbacks may still have cleanup queue entries.
        // Restore must preserve that real engine state, not derive it from flags.
        CKRenderCallbackSnapshot removedTemporary = nullptr;
        Require(CKCaptureRenderCallbacks(ctx, &removedTemporary) == CK_OK, "capture stale cleanup entry");
        Require(CKRestoreRenderCallbacks(ctx, removedTemporary) == CK_OK, "restore stale cleanup entry");
        CKDestroyRenderCallbacks(removedTemporary);
        manager->RemoveAllTemporaryCallbacks();
        callbacks = entity->GetCallbackContainer();
        Require(callbacks->m_PostCallBacks.Size() == 1 && callbacks->m_PostCallBacks[0].callback == reinterpret_cast<void *>(B), "temporary expiration");
        Require(manager->m_TemporaryPreRenderCallbacks.Size() == 0 && manager->m_TemporaryPostRenderCallbacks.Size() == 0, "cleanup queues empty");
        Require(view->m_PostSpriteRenderCallBacks.m_PostCallBacks.Size() == 1, "persistent context callback survives");
    }
    CKRenderCallbackSnapshot overwritten = nullptr;
    Require(CKCaptureRenderCallbacks(ctx, &overwritten) == CK_OK, "new checkpoint after cleanup");
    Require(CKRestoreRenderCallbacks(ctx, snapshot) == CK_OK, "original remains reusable");
    Require(CKRestoreRenderCallbacks(ctx, overwritten) == CK_OK, "overwrite checkpoint");
    Require(entity->GetCallbackContainer()->m_PostCallBacks.Size() == 1, "overwrite content");
    auto *newOwner = static_cast<RCK3dEntity *>(ctx->CreateObject(CKCID_3DENTITY, "later"));
    newOwner->AddPostRenderCallBack(A, &c, TRUE);
    Require(CKRestoreRenderCallbacks(ctx, snapshot) == CK_OK, "restore with unrelated new owner");
    Require(newOwner->GetCallbackContainer()->m_PostCallBacks.Size() == 1, "new owner untouched");
    manager->RemoveAllTemporaryCallbacks();
    Require(newOwner->GetCallbackContainer()->m_PostCallBacks.Size() == 0, "new owner cleanup retained");
    CK_ID deletedId = empty->GetID(); ctx->DestroyObject(deletedId);
    auto *replacement = ctx->CreateObject(CKCID_3DENTITY, "replacement");
    Require(replacement != nullptr, "replacement created");
    const auto *unchanged = entity->GetCallbackContainer();
    Require(CKValidateRenderCallbacks(ctx, snapshot) != CK_OK, "deleted owner invalidates");
    Require(CKRestoreRenderCallbacks(ctx, snapshot) != CK_OK && entity->GetCallbackContainer() == unchanged, "invalid restore changes nothing");
    Require(CKValidateRenderCallbacks(ctx, overwritten) != CK_OK, "all affected slots invalidated");
    CKDestroyRenderCallbacks(snapshot); CKDestroyRenderCallbacks(overwritten);
    CKRenderCallbackSnapshot aba = nullptr;
    Require(CKCaptureRenderCallbacks(ctx, &aba) == CK_OK, "capture ABA checkpoint");
    const CK_ID originalId = replacement->GetID();
    auto *sameAddress = static_cast<RCK3dEntity *>(replacement);
    sameAddress->ModifyObjectFlags(CK_OBJECT_FREEID, 0);
    sameAddress->~RCK3dEntity();
    sameAddress = new(sameAddress) RCK3dEntity(ctx, "same-address replacement");
    // In-place destruction retains the existing class-list slot; FREEID
    // returns its ID to RegisterObject without adding a duplicate index.
    Require(sameAddress->GetID() == originalId, "same ID and address reused");
    Require(CKValidateRenderCallbacks(ctx, aba) != CK_OK, "ABA owner rejected");
    CKDestroyRenderCallbacks(aba);
    CKRenderCallbackSnapshot meshLifetime = nullptr;
    Require(CKCaptureRenderCallbacks(ctx, &meshLifetime) == CK_OK, "capture mesh lifetime");
    ctx->DestroyObject(mesh->GetID());
    Require(CKValidateRenderCallbacks(ctx, meshLifetime) != CK_OK, "deleted mesh rejected");
    CKDestroyRenderCallbacks(meshLifetime);
    CKRenderCallbackSnapshot viewLifetime = nullptr;
    Require(CKCaptureRenderCallbacks(ctx, &viewLifetime) == CK_OK, "capture view lifetime");
    ctx->DestroyObject(view->GetID());
    Require(CKValidateRenderCallbacks(ctx, viewLifetime) != CK_OK, "deleted view rejected");
    CKDestroyRenderCallbacks(viewLifetime);
    CKCloseContext(ctx); Require(CKShutdown() == CK_OK, "shutdown");
    std::printf("RESULT checks=%u failures=0\n", checks);
}
