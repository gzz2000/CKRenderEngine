#include "RCK2dEntity.h"
#include "RCK3dEntity.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "CKRenderCallbackSnapshot.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::fprintf(stderr,"FAIL %s\n",message); std::exit(1); }
}
int main() {
    SetProcessorSpecific_FunctionsPtr();
    Require(CKStartUp()==CK_OK,"startup");
    CKCLASSREGISTERCID(RCKRenderObject, CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCK2dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCK3dEntity, CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCKRenderContext, CKCID_OBJECT);
    CKBuildClassHierarchyTable();
    CKContext *context=nullptr;
    Require(CKCreateContext(&context,nullptr,0,0)==CK_OK,"context");
    auto *manager=new RCKRenderManager(context);
    auto &root=manager->m_SceneGraphRootNode;
    std::array<RCK3dEntity *,3> entities;
    for(auto &e:entities) {e=static_cast<RCK3dEntity *>(context->CreateObject(CKCID_3DENTITY,"particle"));Require(e&&e->m_SceneGraphNode,"node");}
    auto setOrder=[&](std::initializer_list<int> order) {
        for(auto &entry:root.m_TransparentObjects)entry.m_Node->ClearInTransparentList();
        root.m_TransparentObjects.Clear();
        for(int i:order)root.AddTransparentObject(entities[i]->m_SceneGraphNode);
    };
    CKRenderCallbackSnapshot saved=nullptr;
    for(int overwrite=0;overwrite<3;++overwrite) {
        setOrder(overwrite==1?std::initializer_list<int>{1,0}:std::initializer_list<int>{0,1});
        root.m_TransparentObjects[0].m_ZhMin=0.25f;root.m_TransparentObjects[0].m_ZhMax=0.75f;
        const auto first=root.m_TransparentObjects[0];const auto second=root.m_TransparentObjects[1];
        CKDestroyRenderCallbacks(saved);saved=nullptr;
        Require(CKCaptureRenderCallbacks(context,&saved)==CK_OK,"capture overwrite");
        Require(root.m_TransparentObjects[0].m_Node==first.m_Node,"capture preserves order");
        for(int repeat=0;repeat<8;++repeat) {
            setOrder({2,1,0});
            entities[0]->m_SceneGraphNode->SetFlags(CKSGN_OUTSIDEFRUSTUM|CKSGN_BOXVALID);
            Require(CKRestoreRenderCallbacks(context,saved)==CK_OK,"repeat restore");
            Require(root.m_TransparentObjects.Size()==2,"restore membership");
            Require(root.m_TransparentObjects[0].m_Node==first.m_Node&&root.m_TransparentObjects[1].m_Node==second.m_Node,"restore stable transparent order");
            Require(root.m_TransparentObjects[0].m_ZhMin==0.25f&&root.m_TransparentObjects[0].m_ZhMax==0.75f,"restore extent entries");
            Require(entities[0]->m_SceneGraphNode->IsInTransparentList()&&entities[1]->m_SceneGraphNode->IsInTransparentList()&&!entities[2]->m_SceneGraphNode->IsInTransparentList(),"membership flags consistent");
            Require(entities[0]->m_SceneGraphNode->HasAllFlags(CKSGN_OUTSIDEFRUSTUM|CKSGN_BOXVALID),"unrelated flags unchanged");
        }
    }
    auto *view=static_cast<RCKRenderContext *>(context->CreateObject(CKCID_RENDERCONTEXT,"traversal"));
    Require(view!=nullptr,"render context");
    view->m_MaskFree=0; // No entity is eligible for visitation in this pass.
    const CKDWORD fpsCounters[]={0,1,12,0,1,CKDWORD(-1),0,12};
    for(CKDWORD fps:fpsCounters) {
        setOrder({0,1});
        view->m_TimeFpsCalc=fps;
        for(auto &entry:root.m_TransparentObjects)entry.m_Node->m_TimeFpsCalc=fps;
        root.RenderTransparentObjects(view,0);
        Require(root.m_TransparentObjects.Size()==2,"traversal keeps persistent order until filtering");
        for(auto &entry:root.m_TransparentObjects) {
            Require(entry.m_Node->m_TimeFpsCalc!=view->m_TimeFpsCalc,"unvisited marker cannot match reused FPS counter");
            Require(entry.m_Node->IsInTransparentList(),"visitation reset preserves list membership");
        }
    }
    auto *added=static_cast<RCK3dEntity *>(context->CreateObject(CKCID_3DENTITY,"new owner"));
    root.AddTransparentObject(added->m_SceneGraphNode);
    Require(CKRestoreRenderCallbacks(context,saved)==CK_OK,"new owner restore");
    Require(root.m_TransparentObjects.Size()==3&&root.m_TransparentObjects[2].m_Node==added->m_SceneGraphNode,"preserve new owner entry");
    auto *node=entities[0]->m_SceneGraphNode;
    entities[0]->m_SceneGraphNode=nullptr;
    Require(CKValidateRenderCallbacks(context,saved)!=CK_OK,"reject disappeared node");
    Require(CKRestoreRenderCallbacks(context,saved)!=CK_OK,"reject before commit");
    Require(root.m_TransparentObjects.Size()==3&&root.m_TransparentObjects[0].m_Node==node,"failed restore nonmutating");
    entities[0]->m_SceneGraphNode=node;
    CKDestroyRenderCallbacks(saved);saved=nullptr;
    setOrder({});
    Require(CKCaptureRenderCallbacks(context,&saved)==CK_OK,"capture empty list");
    setOrder({0,1,2});
    Require(CKRestoreRenderCallbacks(context,saved)==CK_OK,"restore empty list");
    Require(root.m_TransparentObjects.Size()==0,"future entries removed");
    for(auto *e:entities)Require(!e->m_SceneGraphNode->IsInTransparentList(),"empty membership flags");
    CKDestroyRenderCallbacks(saved);saved=nullptr;
    setOrder({0});root.m_TransparentObjects.PushBack(root.m_TransparentObjects[0]);
    Require(CKCaptureRenderCallbacks(context,&saved)!=CK_OK&&saved==nullptr,"reject duplicate list without handle");
    setOrder({0});node->ClearInTransparentList();
    Require(CKCaptureRenderCallbacks(context,&saved)!=CK_OK&&saved==nullptr,"reject inconsistent membership");
    node->MarkInTransparentList();
    Require(CKCaptureRenderCallbacks(context,&saved)==CK_OK,"capture before owner deletion");
    Require(context->DestroyObject(entities[0]->GetID())==CK_OK,"delete owner");
    Require(CKValidateRenderCallbacks(context,saved)!=CK_OK,"deletion invalidates snapshot");
    CKDestroyRenderCallbacks(saved);
    Require(CKCloseContext(context)==CK_OK,"close");
    Require(CKShutdown()==CK_OK,"shutdown");
    std::printf("PASS %u transparent order checks\n",checks);
}
