#include "RCK2dEntity.h"
#include "RCK3dEntity.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "CKEntityUserBoxState.h"
#include "CKEntityTransformState.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool ok,const char *message) {
    ++checks; if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}
}
static bool Equal(const VxBbox &a,const VxBbox &b) {return !std::memcmp(&a,&b,sizeof(a));}
int main() {
    SetProcessorSpecific_FunctionsPtr(); Require(CKStartUp()==CK_OK,"startup");
    CKCLASSREGISTERCID(RCKRenderObject,CKCID_BEOBJECT);
    CKCLASSREGISTERCID(RCK2dEntity,CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCK3dEntity,CKCID_RENDEROBJECT);
    CKCLASSREGISTERCID(RCKRenderContext,CKCID_OBJECT);
    CKBuildClassHierarchyTable();
    CKContext *ctx=nullptr;Require(CKCreateContext(&ctx,nullptr,0,0)==CK_OK,"context");
    new RCKRenderManager(ctx);
    auto *entity=static_cast<RCK3dEntity *>(ctx->CreateObject(CKCID_3DENTITY,"authored bbox"));
    Require(entity!=nullptr,"entity");
    VxMatrix matrix=VxMatrix::Identity();matrix[3]=VxVector(19.25f,-4.5f,12.75f);
    entity->SetWorldMatrix(matrix);
    VxBbox authored(VxVector(-13.25f,-9.5f,-1.25f),VxVector(5.75f,2.25f,18.5f));
    VxBbox future(VxVector(100,200,300),VxVector(400,500,600));
    entity->SetBoundingBox(&authored,TRUE);
    CKEntityUserBoxState saved;CKEntityTransformState transform;
    const auto beforeFlags=entity->m_MoveableFlags;
    const auto beforeBox=entity->m_WorldBoundingBox;
    Require(CKCaptureEntityUserBoxState(entity,&saved,sizeof(saved)),"capture authored box");
    Require(saved.enabled && Equal(saved.local,authored),"captured local authored values");
    Require(beforeFlags==entity->m_MoveableFlags && Equal(beforeBox,entity->m_WorldBoundingBox),"capture does not update caches");
    Require(CKCaptureEntityTransformState(entity,&transform,sizeof(transform)),"capture transform");
    entity->SetBoundingBox(&future,TRUE);
    Require(CKRestoreEntityTransformState(entity,&transform,sizeof(transform)),"old transform-only restore");
    Require(!Equal(entity->GetBoundingBox(TRUE),authored),"old restore demonstrably retains future user box");
    for(int i=0;i<100;++i) {
        entity->SetBoundingBox(i%2?nullptr:&future,TRUE);
        for(int twice=0;twice<2;++twice) {
            Require(CKRestoreEntityUserBoxState(entity,&saved,sizeof(saved)),"restore repeated authored state");
            Require((entity->m_MoveableFlags & VX_MOVEABLE_USERBOX)!=0,"restore enabled bit");
            // Check before GetBoundingBox: that accessor would repair the
            // flag and hide the first-culling bug in a meshless emitter.
            Require((entity->m_MoveableFlags & VX_MOVEABLE_BOXVALID)!=0,"restored user box eligible for immediate culling");
            VxBbox immediate;immediate.TransformFrom(authored,entity->m_WorldMatrix);
            Require(Equal(entity->m_WorldBoundingBox,immediate),"world box rebuilt before any getter");
            Require(Equal(entity->GetBoundingBox(TRUE),authored),"restore authored geometry");
            VxBbox expected;expected.TransformFrom(authored,entity->m_WorldMatrix);
            Require(Equal(entity->GetBoundingBox(FALSE),expected),"rebuild world box from restored geometry");
        }
        if(i==31 || i==67) {
            authored.Min.x-=0.125f;authored.Max.z+=0.25f;
            entity->SetBoundingBox(&authored,TRUE);
            Require(CKCaptureEntityUserBoxState(entity,&saved,sizeof(saved)),"overwrite capture");
        }
    }
    const auto flags=entity->m_MoveableFlags;const auto local=entity->m_LocalBoundingBox;
    for(int kind=0;kind<3;++kind) {
        auto invalid=saved;
        if(kind==0)invalid.enabled=2;else if(kind==1)invalid.version=99;else invalid.object++;
        Require(!CKRestoreEntityUserBoxState(entity,&invalid,sizeof(invalid)),"reject invalid state");
        Require(flags==entity->m_MoveableFlags && Equal(local,entity->m_LocalBoundingBox),"invalid restore is unchanged");
    }
    entity->SetBoundingBox(nullptr,TRUE);
    CKEntityUserBoxState disabled;
    Require(CKCaptureEntityUserBoxState(entity,&disabled,sizeof(disabled)) && !disabled.enabled,"capture disabled");
    entity->SetBoundingBox(&future,TRUE);
    Require(CKRestoreEntityUserBoxState(entity,&disabled,sizeof(disabled)),"restore disabled");
    Require(!(entity->m_MoveableFlags&VX_MOVEABLE_USERBOX),"future user box no longer active");
    Require(CKCloseContext(ctx)==CK_OK,"close context");Require(CKShutdown()==CK_OK,"shutdown");
    std::printf("PASS %u user bounding-box checks\n",checks);
}
