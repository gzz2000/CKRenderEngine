#include "RCK2dEntity.h"
#include "RCKMaterial.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "CKEntity2dState.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
struct Saved {
    std::vector<CKEntity2dState> states;
    std::vector<std::vector<CK_ID>> children;
    std::vector<CKEntity2dRestoreEntry> Views() const {
        std::vector<CKEntity2dRestoreEntry> result;
        for (std::size_t i=0; i<states.size(); ++i)
            result.push_back({&states[i], children[i].data(), children[i].size()});
        return result;
    }
};
static Saved Capture(CKContext *ctx) {
    Saved saved;
    const XObjectPointerArray objects = ctx->GetObjectListByType(CKCID_2DENTITY, TRUE);
    for (auto *object : objects) {
        CKEntity2dState state;
        Require(CKCaptureEntity2dState(object, &state, sizeof(state)), "capture");
        saved.states.push_back(state);
        saved.children.emplace_back();
        auto *entity = CK2dEntity::Cast(object);
        for (int i=0; i<entity->GetChildrenCount(); ++i)
            saved.children.back().push_back(entity->GetChild(i)->GetID());
    }
    return saved;
}
static bool Equal(const Saved &a, const Saved &b) {
    if (a.states.size()!=b.states.size() || a.children!=b.children) return false;
    for (std::size_t i=0; i<a.states.size(); ++i) {
        const auto &x=a.states[i], &y=b.states[i];
        if (x.object!=y.object || x.parent!=y.parent || x.material!=y.material ||
            x.flags!=y.flags || x.zOrder!=y.zOrder || x.hasHomogeneous!=y.hasHomogeneous ||
            std::memcmp(&x.rect,&y.rect,sizeof(VxRect)) ||
            std::memcmp(&x.source,&y.source,sizeof(VxRect)) ||
            std::memcmp(&x.vertices,&y.vertices,sizeof(VxRect)) ||
            std::memcmp(&x.sourceCache,&y.sourceCache,sizeof(VxRect)) ||
            (x.hasHomogeneous && std::memcmp(&x.homogeneous,&y.homogeneous,sizeof(VxRect)))) return false;
    }
    return true;
}
static int Restore(CKContext *ctx, const Saved &saved) {
    auto entries=saved.Views();
    return CKRestoreEntity2dStates(ctx,entries.data(),entries.size(),sizeof(entries[0]));
}
int main() {
    SetProcessorSpecific_FunctionsPtr();
    Require(CKStartUp()==CK_OK,"startup");
    CKCLASSREGISTERCID(RCKRenderObject, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCK2dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCKMaterial, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCKRenderContext, CKCID_OBJECT);
    CKBuildClassHierarchyTable();
    CKContext *ctx=nullptr;
    Require(CKCreateContext(&ctx,nullptr,0,0)==CK_OK,"context");
    auto *manager=new RCKRenderManager(ctx);
    auto *render=static_cast<RCKRenderContext *>(ctx->CreateObject(CKCID_RENDERCONTEXT,"test viewport"));
    Require(render && manager->m_2DRootFore && manager->m_2DRootBack,"roots and context");
    ctx->m_BehaviorContext.CurrentRenderContext=render;
    auto resize=[&](int width,int height) {
        render->m_ViewportData.ViewX=render->m_ViewportData.ViewY=0;
        render->m_ViewportData.ViewWidth=width;render->m_ViewportData.ViewHeight=height;
        render->m_Settings.m_Rect={0,0,width,height};
        manager->m_2DRootFore->SetRect(VxRect(0,0,float(width),float(height)),TRUE);
        manager->m_2DRootBack->SetRect(VxRect(0,0,float(width),float(height)),TRUE);
    };
    resize(853,479);
    std::vector<CK2dEntity *> entities;
    for(int i=0;i<14;++i) {
        auto *entity=CK2dEntity::Cast(ctx->CreateObject(CKCID_2DENTITY,"equal-Z snapshot test"));
        Require(entity!=nullptr,"create entity");
        entities.push_back(entity);
        entity->SetParent(i<7?manager->m_2DRootFore:entities[0]);
        entity->SetRect(VxRect(13.1f+i,9.3f+i,133.4f+i,114.7f+i),TRUE);
        if(i%2) {
            entity->SetFlags(entity->GetFlags()|CK_2DENTITY_USEHOMOGENEOUSCOORD);
            entity->SetHomogeneousRect(VxRect(.132751f,.216134f,.641132f,.812375f),TRUE);
        }
        entity->SetZOrder(7);
    }
    Require(entities[0]->GetParent()==nullptr,"render-root child has null parent");
    Saved saved=Capture(ctx);
    // Replaying the old public-setter strategy must actually expose the
    // regression with this fixture, rather than testing a trivial no-op.
    for(auto *entity:entities) if(entity->GetParent()) entity->SetParent(nullptr);
    for(const auto &state:saved.states) {
        auto *entity=CK2dEntity::Cast(ctx->GetObject(state.object));
        if(state.parent) entity->SetParent(CK2dEntity::Cast(ctx->GetObject(state.parent)));
        entity->SetFlags(state.flags);
        if(state.hasHomogeneous) entity->SetHomogeneousRect(state.homogeneous,TRUE);
        else entity->SetRect(state.rect,TRUE);
        entity->SetZOrder(state.zOrder);
    }
    Require(!Equal(saved,Capture(ctx)),"old setter replay reproduces state loss");
    Require(Restore(ctx,saved),"exact graph restore");
    Require(Equal(saved,Capture(ctx)),"exact graph and both coordinate systems");
    for(int iteration=0;iteration<100;++iteration) {
        if(iteration==17 || iteration==63) {
            entities[1]->SetHomogeneousRect(VxRect(.173451f,.272512f,.725733f,.929113f),TRUE);
            saved=Capture(ctx);
        }
        for(auto *entity:entities) {
            entity->SetParent(manager->m_2DRootBack);
            entity->SetZOrder(300-iteration);
            entity->SetFlags(entity->GetFlags()&~CK_2DENTITY_USEHOMOGENEOUSCOORD);
            entity->SetRect(VxRect(0,0,1,1),TRUE);
        }
        Require(Restore(ctx,saved),"restore after reparent/coordinate mode change");
        Require(Equal(saved,Capture(ctx)),"exact first restore");
        Require(Restore(ctx,saved),"restore twice");
        Require(Equal(saved,Capture(ctx)),"idempotent second restore");
    }
    for(int failure=0;failure<5;++failure) {
        auto bad=saved;
        if(failure==0) bad.states.back().object=0xffffffffu;
        if(failure==1) bad.states.back().parent=0xffffffffu;
        if(failure==2) bad.states.back().version=99;
        if(failure==3) {
            for(auto &children:bad.children) if(!children.empty()) { children.push_back(children[0]);break; }
        }
        if(failure==4) {
            bad.states.back().flags|=CK_2DENTITY_USEHOMOGENEOUSCOORD;
            bad.states.back().hasHomogeneous=0;
        }
        auto before=Capture(ctx);
        Require(!Restore(ctx,bad),"reject invalid graph/state");
        Require(Equal(before,Capture(ctx)),"rejection is atomic");
    }
    resize(1280,720);
    Require(Restore(ctx,saved),"restore after viewport resize");
    VxRect rect;manager->m_2DRootFore->GetRect(rect);
    Require(rect==VxRect(0,0,1280,720),"keep live viewport");
    const auto resized=Capture(ctx);
    entities[1]->GetRect(rect);
    VxRect hom;Require(entities[1]->GetHomogeneousRect(hom)==CK_OK,"homogeneous root child");
    VxRect expected=hom;expected.TransformFromHomogeneous(VxRect(0,0,1280,720));
    Require(std::memcmp(&rect,&expected,sizeof(rect))==0,"root child follows live viewport");
    entities[9]->GetRect(rect);Require(entities[9]->GetHomogeneousRect(hom)==CK_OK,"nested homogeneous child");
    VxRect parent;entities[0]->GetRect(parent);expected=hom;expected.TransformFromHomogeneous(parent);
    Require(std::memcmp(&rect,&expected,sizeof(rect))==0,"nested child uses final parent rect");
    for(int i=0;i<8;++i) {
        Require(Restore(ctx,saved),"repeat resized restore");
        Require(Equal(resized,Capture(ctx)),"resized restore has no drift");
    }
    resize(853,479);
    Require(Restore(ctx,saved),"return to original viewport");
    Require(Equal(saved,Capture(ctx)),"original layout preserved across resize");
    ctx->m_BehaviorContext.CurrentRenderContext=nullptr;
    Require(CKCloseContext(ctx)==CK_OK,"close");
    Require(CKShutdown()==CK_OK,"shutdown");
    std::printf("RESULT checks=%u failures=0\n",checks);
}
