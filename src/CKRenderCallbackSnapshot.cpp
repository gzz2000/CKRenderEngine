#include "CKRenderCallbackSnapshot.h"
#include "CKRenderCallbackSnapshotInternal.h"
#include "CKContext.h"
#include "RCKRenderObject.h"
#include "RCKMesh.h"
#include "RCK3dEntity.h"
#include "RCKRenderContext.h"
#include "RCKRenderManager.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

struct CKRenderCallbackSnapshotAccess {
    struct Slot {
        CKCallbacksContainer **owned = nullptr;
        CKCallbacksContainer *embedded = nullptr;
        CKCallbacksContainer *Get() const { return owned ? *owned : embedded; }
        void Commit(std::unique_ptr<CKCallbacksContainer> &prepared) const {
            if (owned) {
                CKCallbacksContainer *old = *owned;
                *owned = prepared.release();
                prepared.reset(old);
            } else {
                embedded->m_PreCallBacks.Swap(prepared->m_PreCallBacks);
                embedded->m_PostCallBacks.Swap(prepared->m_PostCallBacks);
                std::swap(embedded->m_Callback, prepared->m_Callback);
            }
        }
    };
    static std::vector<Slot> Slots(CKObject *object) {
        if (CKIsChildClassOf(object, CKCID_RENDEROBJECT)) {
            return {{&static_cast<RCKRenderObject *>(object)->m_Callbacks, nullptr}};
        }
        if (CKIsChildClassOf(object, CKCID_MESH)) {
            auto *mesh = static_cast<RCKMesh *>(object);
            return {{&mesh->m_RenderCallbacks, nullptr}, {&mesh->m_SubMeshCallbacks, nullptr}};
        }
        if (CKIsChildClassOf(object, CKCID_RENDERCONTEXT)) {
            auto *context = static_cast<RCKRenderContext *>(object);
            return {{nullptr, &context->m_PreRenderCallBacks},
                    {nullptr, &context->m_PostRenderCallBacks},
                    {nullptr, &context->m_PostSpriteRenderCallBacks}};
        }
        return {};
    }
};

namespace {
struct ContainerState {
    bool allocated = false;
    std::vector<VxCallBack> pre, post;
    bool hasMain = false;
    VxCallBack main{};
};
struct OwnerState {
    CK_ID id;
    CKObject *identity;
    CK_CLASSID classId;
    std::vector<ContainerState> containers;
};
struct QueueEntry {
    std::size_t owner, slot;
    void *function;
    void *argument;
};
struct TransparentState {
    CK_ID id;
    RCK3dEntity *identity;
    CKTransparentObject entry;
};
struct Snapshot {
    CKContext *context = nullptr;
    RCKRenderManager *manager = nullptr;
    bool valid = true;
    std::vector<OwnerState> owners;
    std::vector<QueueEntry> preQueue, postQueue;
    std::vector<TransparentState> transparentOrder;
};
std::unordered_set<Snapshot *> snapshots;

ContainerState CaptureContainer(const CKCallbacksContainer *container) {
    ContainerState state;
    if (!container) return state;
    state.allocated = true;
    for (const auto &cb : container->m_PreCallBacks) state.pre.push_back(cb);
    for (const auto &cb : container->m_PostCallBacks) state.post.push_back(cb);
    if (container->m_Callback) {
        state.hasMain = true;
        state.main = *container->m_Callback;
    }
    return state;
}
std::unique_ptr<CKCallbacksContainer> PrepareContainer(const ContainerState &state) {
    if (!state.allocated) return nullptr;
    auto result = std::make_unique<CKCallbacksContainer>();
    for (const auto &cb : state.pre) result->m_PreCallBacks.PushBack(cb);
    for (const auto &cb : state.post) result->m_PostCallBacks.PushBack(cb);
    if (state.hasMain) result->m_Callback = new VxCallBack(state.main);
    return result;
}
}

void CKInvalidateRenderCallbackOwner(CKObject *object) {
    for (auto *snapshot : snapshots) {
        if (!snapshot->valid) continue;
        for (const auto &owner : snapshot->owners) {
            if (owner.identity == object) {
                snapshot->valid = false;
                break;
            }
        }
    }
}
void CKInvalidateRenderCallbackContext(CKContext *context) {
    for (auto *snapshot : snapshots)
        if (snapshot->context == context) snapshot->valid = false;
}

int CKCaptureRenderCallbacks(CKContext *context, CKRenderCallbackSnapshot *handle) {
    if (!context || !handle) return CKERR_INVALIDPARAMETER;
    *handle = nullptr;
    auto *manager = static_cast<RCKRenderManager *>(context->GetRenderManager());
    if (!manager) return CKERR_INVALIDOBJECT;
    try {
        auto result = std::make_unique<Snapshot>();
        result->context = context;
        result->manager = manager;
        std::unordered_map<const CKCallbacksContainer *, std::pair<std::size_t, std::size_t>> addresses;
        const auto &objects = context->GetObjectListByType(CKCID_OBJECT, TRUE);
        for (auto *object : objects) {
            auto slots = CKRenderCallbackSnapshotAccess::Slots(object);
            if (slots.empty()) continue;
            if (object->IsToBeDeleted()) return CKERR_INVALIDOBJECT;
            OwnerState owner{object->GetID(), object, object->GetClassID(), {}};
            for (const auto &slot : slots) {
                if (slot.Get()) addresses.emplace(slot.Get(), std::make_pair(result->owners.size(), owner.containers.size()));
                owner.containers.push_back(CaptureContainer(slot.Get()));
            }
            result->owners.push_back(std::move(owner));
        }
        const auto captureQueue = [&](const auto &queue, auto &saved) {
            for (const auto &cb : queue) {
                auto found = addresses.find(static_cast<CKCallbacksContainer *>(cb.callback));
                if (found == addresses.end()) return false;
                saved.push_back(QueueEntry{found->second.first, found->second.second, cb.argument, cb.arg2});
            }
            return true;
        };
        if (!captureQueue(manager->m_TemporaryPreRenderCallbacks, result->preQueue) ||
            !captureQueue(manager->m_TemporaryPostRenderCallbacks, result->postQueue)) return CKERR_INVALIDOBJECT;
        // The transparent list survives frames. Its stable tie order is
        // observable through blending, even though depth extents are recomputed.
        // Capture the order rather than inventing an ID-based sort on restore.
        std::unordered_set<CKSceneGraphNode *> listed;
        for (const auto &entry : manager->m_SceneGraphRootNode.m_TransparentObjects) {
            auto *node = entry.m_Node;
            auto *entity = node ? node->m_Entity : nullptr;
            if (!entity || entity->IsToBeDeleted() || entity->m_SceneGraphNode != node ||
                !node->IsInTransparentList() || !listed.insert(node).second)
                return CKERR_INVALIDOBJECT;
            result->transparentOrder.push_back({entity->GetID(), entity, entry});
        }
        snapshots.insert(result.get());
        *handle = result.release();
        return CK_OK;
    } catch (...) { return CKERR_OUTOFMEMORY; }
}

int CKValidateRenderCallbacks(CKContext *context, CKRenderCallbackSnapshot handle) {
    auto *saved = static_cast<Snapshot *>(handle);
    if (!context || !saved || !snapshots.count(saved)) return CKERR_INVALIDPARAMETER;
    if (!saved->valid || saved->context != context || context->GetRenderManager() != saved->manager)
        return CKERR_INVALIDOBJECT;
    for (const auto &owner : saved->owners) {
        auto *current = context->GetObject(owner.id);
        if (current != owner.identity || !current || current->IsToBeDeleted() || current->GetClassID() != owner.classId)
            return CKERR_INVALIDOBJECT;
    }
    for (const auto &entry : saved->transparentOrder) {
        auto *current = context->GetObject(entry.id);
        if (current != entry.identity || !current || current->IsToBeDeleted() ||
            entry.identity->m_SceneGraphNode != entry.entry.m_Node)
            return CKERR_INVALIDOBJECT;
    }
    return CK_OK;
}

int CKRestoreRenderCallbacks(CKContext *context, CKRenderCallbackSnapshot handle) {
    const int validation = CKValidateRenderCallbacks(context, handle);
    if (validation != CK_OK) return validation;
    auto &saved = *static_cast<Snapshot *>(handle);
    try {
        struct PreparedOwner {
            std::vector<CKRenderCallbackSnapshotAccess::Slot> slots;
            std::vector<std::unique_ptr<CKCallbacksContainer>> replacements;
        };
        std::vector<PreparedOwner> prepared;
        std::unordered_set<const CKCallbacksContainer *> replaced;
        for (const auto &owner : saved.owners) {
            PreparedOwner item;
            item.slots = CKRenderCallbackSnapshotAccess::Slots(owner.identity);
            if (item.slots.size() != owner.containers.size()) return CKERR_INVALIDOBJECT;
            for (std::size_t i = 0; i < item.slots.size(); ++i) {
                if (item.slots[i].Get()) replaced.insert(item.slots[i].Get());
                item.replacements.push_back(PrepareContainer(owner.containers[i]));
            }
            prepared.push_back(std::move(item));
        }
        XClassArray<VxCallBack> preQueue, postQueue;
        const auto prepareQueue = [&](const auto &entries, const auto &current, auto &target) {
            for (const auto &entry : entries) {
                const auto &item = prepared[entry.owner];
                const auto &slot = item.slots[entry.slot];
                VxCallBack cb{};
                cb.callback = slot.owned ? item.replacements[entry.slot].get() : slot.embedded;
                cb.argument = entry.function;
                cb.arg2 = entry.argument;
                target.PushBack(cb);
            }
            // Owners created since capture are reconciled by the world lifetime
            // layer. Do not lose their independent temporary cleanup entries.
            for (const auto &cb : current)
                if (!replaced.count(static_cast<CKCallbacksContainer *>(cb.callback))) target.PushBack(cb);
        };
        prepareQueue(saved.preQueue, saved.manager->m_TemporaryPreRenderCallbacks, preQueue);
        prepareQueue(saved.postQueue, saved.manager->m_TemporaryPostRenderCallbacks, postQueue);
        XClassArray<CKTransparentObject> transparentOrder;
        std::unordered_set<CKObject *> capturedOwners;
        for (const auto &owner : saved.owners) capturedOwners.insert(owner.identity);
        for (const auto &entry : saved.transparentOrder) transparentOrder.PushBack(entry.entry);
        // As with callback queues, newly created owners remain the lifetime
        // layer's responsibility. Preserve their independent live entries.
        for (const auto &entry : saved.manager->m_SceneGraphRootNode.m_TransparentObjects) {
            if (!entry.m_Node || !entry.m_Node->m_Entity) return CKERR_INVALIDOBJECT;
            if (!capturedOwners.count(entry.m_Node->m_Entity)) transparentOrder.PushBack(entry);
        }
        // All allocations and validation have completed. Commit via swaps only.
        for (auto &item : prepared)
            for (std::size_t i = 0; i < item.slots.size(); ++i) item.slots[i].Commit(item.replacements[i]);
        saved.manager->m_TemporaryPreRenderCallbacks.Swap(preQueue);
        saved.manager->m_TemporaryPostRenderCallbacks.Swap(postQueue);
        auto &currentOrder = saved.manager->m_SceneGraphRootNode.m_TransparentObjects;
        for (const auto &entry : currentOrder) entry.m_Node->ClearInTransparentList();
        currentOrder.Swap(transparentOrder);
        for (const auto &entry : currentOrder) entry.m_Node->MarkInTransparentList();
        return CK_OK;
    } catch (...) { return CKERR_OUTOFMEMORY; }
}

void CKDestroyRenderCallbacks(CKRenderCallbackSnapshot handle) {
    auto *saved = static_cast<Snapshot *>(handle);
    if (snapshots.erase(saved)) delete saved;
}
