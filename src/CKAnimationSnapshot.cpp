#include "CKAnimationSnapshot.h"
#include "CKContext.h"
#include "CKObjectLifetime.h"
#include "RCKObjectAnimation.h"
#include "RCKKeyedAnimation.h"
#include "RCKCharacter.h"
#include "RCK3dEntity.h"
#include <climits>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Access is confined to this translation unit; no object layout/vtable changes.
struct CKAnimationSnapshotAccess {
    struct ObjectState {
        RCKObjectAnimation *object;
        CKKeyframeData *dataIdentity;
        CKDWORD flags, reserved;
        RCK3dEntity *entity;
        float step, merge;
        RCKObjectAnimation *first, *second;
        RCKKeyedAnimation *parent;
    };
    struct MasterState {
        RCKAnimation *object;
        RCKCharacter *character;
        float length, step, frameRate;
        RCK3dEntity *root;
        CKDWORD flags;
        RCKKeyedAnimation *keyed;
        std::vector<CKObject *> children;
        CKBOOL merged = FALSE;
        float merge = 0;
        RCKObjectAnimation *rootAnimation = nullptr;
        VxVector center;
    };
    static ObjectState Read(RCKObjectAnimation *p) {
        return {p,p->m_KeyframeData,p->m_Flags,p->m_field_38,p->m_Entity,
            p->m_CurrentStep,p->m_MergeFactor,p->m_Anim1,p->m_Anim2,p->m_ParentKeyedAnimation};
    }
    static MasterState Read(RCKAnimation *p) {
        MasterState s{};
        s.object=p;s.character=p->m_Character;s.length=p->m_Length;
        s.step=p->m_Step;s.frameRate=p->m_FrameRate;s.root=p->m_RootEntity;s.flags=p->m_Flags;
        if (CKIsChildClassOf(p,CKCID_KEYEDANIMATION)) {
            auto *k=static_cast<RCKKeyedAnimation *>(p);s.keyed=k;
            for (auto *child:k->m_Animations) s.children.push_back(child);
            s.merged=k->m_Merged;s.merge=k->m_MergeFactor;
            s.rootAnimation=k->m_RootAnimation;s.center=k->m_Vector;
        }
        return s;
    }
    template<class C> static void CloneTangents(C *to, C *from) {
        if (!from->m_Tangents) return;
        if (from->m_NbKeys<2 || from->m_NbKeys>INT_MAX/2)
            throw std::runtime_error("Invalid animation tangent count");
        using T=std::remove_pointer_t<decltype(from->m_Tangents)>;
        to->m_Tangents=new T[static_cast<size_t>(from->m_NbKeys)*2];
        std::memcpy(to->m_Tangents,from->m_Tangents,sizeof(T)*from->m_NbKeys*2);
    }
    static CKAnimController *CloneController(CKAnimController *from) {
        if (!from) return nullptr;
        CKKeyframeData factory;
        std::unique_ptr<CKAnimController> to(factory.CreateController(
            static_cast<CKANIMATION_CONTROLLER>(from->GetType())));
        if (!to || !to->Clone(from)) throw std::runtime_error("Cannot clone animation controller");
        // Public Clone deliberately invalidates lazy caches. A checkpoint must
        // retain even a stale cache: direct key edits are observable on the next
        // Evaluate. Capture must neither recompute nor warm the original cache.
        switch (from->GetType()) {
#define TCB_CASE(type,cls) case type: CloneTangents(static_cast<cls *>(to.get()),static_cast<cls *>(from));break
        TCB_CASE(CKANIMATION_TCBPOS_CONTROL,RCKTCBPositionController);
        TCB_CASE(CKANIMATION_TCBROT_CONTROL,RCKTCBRotationController);
        TCB_CASE(CKANIMATION_TCBSCL_CONTROL,RCKTCBScaleController);
        TCB_CASE(CKANIMATION_TCBSCLAXIS_CONTROL,RCKTCBScaleAxisController);
#undef TCB_CASE
        case CKANIMATION_BEZIERPOS_CONTROL:
            static_cast<RCKBezierPositionController *>(to.get())->m_TangentsComputed=
                static_cast<RCKBezierPositionController *>(from)->m_TangentsComputed;break;
        case CKANIMATION_BEZIERSCL_CONTROL:
            static_cast<RCKBezierScaleController *>(to.get())->m_TangentsComputed=
                static_cast<RCKBezierScaleController *>(from)->m_TangentsComputed;break;
        }
        return to.release();
    }
    static std::unique_ptr<CKKeyframeData> CloneData(CKKeyframeData *from) {
        auto to=std::make_unique<CKKeyframeData>();
        to->m_PositionController=CloneController(from->m_PositionController);
        to->m_ScaleController=CloneController(from->m_ScaleController);
        to->m_RotationController=CloneController(from->m_RotationController);
        to->m_ScaleAxisController=CloneController(from->m_ScaleAxisController);
        to->m_MorphController=static_cast<CKMorphController *>(CloneController(from->m_MorphController));
        to->m_Length=from->m_Length;to->m_RefCount=from->m_RefCount;
        to->m_ObjectAnimation=from->m_ObjectAnimation;
        return to;
    }
    static void Release(RCKObjectAnimation *p) { p->ReleaseKeyframeData(); }
    static void Write(const ObjectState &s, CKKeyframeData *data) {
        auto *p=s.object;p->m_KeyframeData=data;p->m_Flags=s.flags;p->m_field_38=s.reserved;
        p->m_Entity=s.entity;p->m_CurrentStep=s.step;p->m_MergeFactor=s.merge;
        p->m_Anim1=s.first;p->m_Anim2=s.second;p->m_ParentKeyedAnimation=s.parent;
    }
    static void Write(const MasterState &s, XSObjectPointerArray &prepared) {
        auto *p=s.object;p->m_Character=s.character;p->m_Length=s.length;
        p->m_Step=s.step;p->m_FrameRate=s.frameRate;p->m_RootEntity=s.root;p->m_Flags=s.flags;
        if (s.keyed) {
            auto *k=s.keyed;k->m_Animations.Swap(prepared);k->m_Merged=s.merged;
            k->m_MergeFactor=s.merge;k->m_RootAnimation=s.rootAnimation;k->m_Vector=s.center;
        }
    }
};

namespace {
using Access=CKAnimationSnapshotAccess;
struct Identity { CK_ID id; CKObject *address; CK_CLASSID cid; std::uint64_t token; };
struct EntityState {
    RCK3dEntity *object;
    bool allocated;
    std::vector<CKObject *> channels;
};
struct DataState {
    CKKeyframeData *identity; // An opaque map key only, never dereferenced on restore.
    std::unique_ptr<CKKeyframeData> data;
};
struct Snapshot {
    CKContext *context;
    std::vector<Identity> identities;
    std::unordered_set<CKObject *> captured;
    std::vector<Access::ObjectState> objects;
    std::vector<Access::MasterState> masters;
    std::vector<EntityState> entities;
    std::vector<DataState> data;
    void Remember(CKObject *p) {
        if (!p || !captured.insert(p).second) return;
        auto token=CKGetObjectLifetimeToken(context,p->GetID());
        if (!token || context->GetObject(p->GetID())!=p)
            throw std::runtime_error("Invalid animation reference");
        identities.push_back({p->GetID(),p,p->GetClassID(),token});
    }
};
struct Prepared {
    Snapshot *source;
    bool consumed=false;
    std::vector<std::unique_ptr<CKKeyframeData>> data;
    std::vector<CKKeyframeData *> assigned;
    std::vector<std::unique_ptr<XObjectPointerArray>> entities;
    std::vector<std::unique_ptr<XSObjectPointerArray>> masters;
};
std::unordered_set<Snapshot *> snapshots;
std::unordered_set<Prepared *> preparedRestores;
int Validate(CKContext *context, Snapshot *s) {
    if (!context || !snapshots.count(s)) return CKERR_INVALIDPARAMETER;
    if (s->context!=context) return CKERR_INVALIDOBJECT;
    for (const auto &i:s->identities) {
        auto *p=context->GetObject(i.id);
        if (p!=i.address || CKGetObjectLifetimeToken(context,i.id)!=i.token ||
            p->GetClassID()!=i.cid) return CKERR_INVALIDOBJECT;
    }
    return CK_OK;
}
}

int CKCaptureAnimationState(CKContext *context, void **handle) {
    if (!context || !handle) return CKERR_INVALIDPARAMETER;
    *handle=nullptr;
    try {
        auto s=std::make_unique<Snapshot>();s->context=context;
        std::unordered_map<CKKeyframeData *,size_t> groups;
        std::unordered_map<CKKeyframeData *,int> refs;
        for (auto *p:context->GetObjectListByType(CKCID_OBJECT,TRUE)) {
            if (CKIsChildClassOf(p,CKCID_OBJECTANIMATION)) {
                s->Remember(p);auto o=Access::Read(static_cast<RCKObjectAnimation *>(p));
                s->Remember(o.entity);s->Remember(o.first);s->Remember(o.second);s->Remember(o.parent);
                s->objects.push_back(o);
                if (o.dataIdentity) {
                    ++refs[o.dataIdentity];
                    if (!groups.count(o.dataIdentity)) {
                        groups[o.dataIdentity]=s->data.size();
                        s->Remember(o.dataIdentity->m_ObjectAnimation);
                        s->data.push_back({o.dataIdentity,Access::CloneData(o.dataIdentity)});
                    }
                }
            }
            if (CKIsChildClassOf(p,CKCID_ANIMATION)) {
                s->Remember(p);auto m=Access::Read(static_cast<RCKAnimation *>(p));
                s->Remember(m.character);s->Remember(m.root);s->Remember(m.rootAnimation);
                for (auto *child:m.children) s->Remember(child);
                s->masters.push_back(std::move(m));
            }
            if (CKIsChildClassOf(p,CKCID_3DENTITY)) {
                s->Remember(p);auto *e=static_cast<RCK3dEntity *>(p);
                EntityState es{e,e->m_ObjectAnimations!=nullptr,{}};
                if (e->m_ObjectAnimations) for (auto *child:*e->m_ObjectAnimations) {
                    s->Remember(child);es.channels.push_back(child);
                }
                s->entities.push_back(std::move(es));
            }
        }
        // Sharing is an alias graph, not one independent controller copy per
        // animation. Refcounts must describe exactly the registered sharers.
        for (const auto &g:s->data) {
            if (refs[g.identity]!=g.data->m_RefCount ||
                (g.data->m_ObjectAnimation &&
                 Access::Read(static_cast<RCKObjectAnimation *>(g.data->m_ObjectAnimation)).dataIdentity!=g.identity))
                return CKERR_INVALIDOBJECT;
        }
        snapshots.insert(s.get());*handle=s.release();return CK_OK;
    } catch (const std::bad_alloc &) { return CKERR_OUTOFMEMORY; }
      catch (...) { return CKERR_INVALIDOBJECT; }
}

int CKPrepareAnimationRestore(CKContext *context, void *handle, void **result) {
    if (!result) return CKERR_INVALIDPARAMETER;
    *result=nullptr;auto *s=static_cast<Snapshot *>(handle);
    int v=Validate(context,s);if (v!=CK_OK) return v;
    try {
        auto p=std::make_unique<Prepared>();p->source=s;
        std::unordered_map<CKKeyframeData *,CKKeyframeData *> mapped;
        for (const auto &g:s->data) {
            p->data.push_back(Access::CloneData(g.data.get()));
            mapped.emplace(g.identity,p->data.back().get());
        }
        for (const auto &o:s->objects) p->assigned.push_back(o.dataIdentity?mapped.at(o.dataIdentity):nullptr);
        for (const auto &e:s->entities) {
            std::unique_ptr<XObjectPointerArray> list;
            if (e.allocated) {
                list=std::make_unique<XObjectPointerArray>();list->Reserve(static_cast<int>(e.channels.size()));
                for (auto *o:e.channels) list->PushBack(o);
            }
            p->entities.push_back(std::move(list));
        }
        for (const auto &m:s->masters) {
            auto list=std::make_unique<XSObjectPointerArray>();list->Resize(static_cast<int>(m.children.size()));
            for (size_t i=0;i<m.children.size();++i) (*list)[static_cast<int>(i)]=m.children[i];
            p->masters.push_back(std::move(list));
        }
        preparedRestores.insert(p.get());*result=p.release();return CK_OK;
    } catch (const std::bad_alloc &) { return CKERR_OUTOFMEMORY; }
      catch (...) { return CKERR_INVALIDOBJECT; }
}

int CKCommitAnimationRestore(CKContext *context, void *handle) {
    auto *p=static_cast<Prepared *>(handle);
    if (!preparedRestores.count(p) || p->consumed) return CKERR_INVALIDPARAMETER;
    auto *s=p->source;int v=Validate(context,s);if (v!=CK_OK) return v;
    // A new sharer or entity could retain reciprocal references into the saved
    // graph. Reconcile world lifetimes first, rather than leave a half-old graph.
    for (auto *o:context->GetObjectListByType(CKCID_OBJECT,TRUE))
        if ((CKIsChildClassOf(o,CKCID_OBJECTANIMATION) || CKIsChildClassOf(o,CKCID_ANIMATION) ||
             CKIsChildClassOf(o,CKCID_3DENTITY)) && !s->captured.count(o)) return CKERR_INVALIDOBJECT;
    for (const auto &o:s->objects) Access::Release(o.object);
    for (size_t i=0;i<s->objects.size();++i) Access::Write(s->objects[i],p->assigned[i]);
    for (auto &data:p->data) data.release(); // Captured alias groups now own them.
    for (size_t i=0;i<s->entities.size();++i) {
        auto *e=s->entities[i].object;
        auto *old=e->m_ObjectAnimations;e->m_ObjectAnimations=p->entities[i].release();p->entities[i].reset(old);
    }
    for (size_t i=0;i<s->masters.size();++i) Access::Write(s->masters[i],*p->masters[i]);
    p->consumed=true;return CK_OK;
}
void CKDestroyAnimationRestore(void *handle) {
    auto *p=static_cast<Prepared *>(handle);
    if (preparedRestores.erase(p)) delete p;
}
void CKDestroyAnimationState(void *handle) {
    auto *s=static_cast<Snapshot *>(handle);
    if (snapshots.erase(s)) delete s;
}
