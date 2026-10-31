#include "CKAll.h"
#include "RCKRenderManager.h"
#include "RCKRenderContext.h"
#include "RCKSprite3D.h"
#include "CKRenderedScene.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern void SetProcessorSpecific_FunctionsPtr();
static unsigned checks;
static void Require(bool ok,const char *message){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
int main(){
 SetProcessorSpecific_FunctionsPtr();Require(CKStartUp()==CK_OK,"startup");
 CKCLASSREGISTERCID(RCKRenderObject,CKCID_BEOBJECT);
 CKCLASSREGISTERCID(RCK3dEntity,CKCID_RENDEROBJECT);
 CKCLASSREGISTERCID(RCKSprite3D,CKCID_3DENTITY);
 CKCLASSREGISTERCID(RCKRenderContext,CKCID_OBJECT);
 CKBuildClassHierarchyTable();
 CKContext *ctx=nullptr;Require(CKCreateContext(&ctx,nullptr,0,0)==CK_OK,"context");
 auto *manager=new RCKRenderManager(ctx);(void)manager;
 auto *view=static_cast<RCKRenderContext*>(ctx->CreateObject(CKCID_RENDERCONTEXT,"sprite test"));
 auto *parent=static_cast<RCK3dEntity*>(ctx->CreateObject(CKCID_3DENTITY,"translated parent"));
 auto *sprite=static_cast<RCKSprite3D*>(ctx->CreateObject(CKCID_SPRITE3D,"orbit billboard"));
 auto *child=static_cast<RCK3dEntity*>(ctx->CreateObject(CKCID_3DENTITY,"frame child"));
 Require(view&&parent&&sprite&&child&&view->m_RenderedScene->GetRootEntity(),"owners");
 VxVector parentPosition(10000.3f,-221.75f,573.6f);parent->SetPosition(&parentPosition);
 sprite->SetParent(reinterpret_cast<CK3dEntity*>(parent),TRUE);child->SetParent(reinterpret_cast<CK3dEntity*>(sprite),TRUE);
 for(auto mode:{VXSPRITE3D_BILLBOARD,VXSPRITE3D_XROTATE,VXSPRITE3D_YROTATE,VXSPRITE3D_ORIENTABLE}){
  sprite->SetMode(mode);
  VxMatrix local=VxMatrix::Identity();local[3].Set(1.1234567f,-0.966906f,1.432571f);sprite->SetLocalMatrix(local);
  const VxVector savedLocal=sprite->m_LocalMatrix[3],savedWorld=sprite->m_WorldMatrix[3];
  const VxMatrix childWorld=child->m_WorldMatrix;
  for(int repeat=0;repeat<32;++repeat){
   VxMatrix camera=VxMatrix::Identity();camera[0].Set(0,0,1);camera[1].Set(0.6f,0.8f,0);camera[2].Set(-0.8f,0.6f,0);
   if(repeat%2){camera[0].Set(0,0,-1);camera[1].Set(-0.6f,0.8f,0);camera[2].Set(0.8f,0.6f,0);}
   auto *root=reinterpret_cast<RCK3dEntity*>(view->m_RenderedScene->GetRootEntity());root->SetWorldMatrix(camera);
   const VxMatrix oldWorld=sprite->m_WorldMatrix;
   sprite->UpdateCameraFacingOrientation(view);
   Require(!std::memcmp(&savedLocal,&sprite->m_LocalMatrix[3],sizeof(VxVector)),"facing preserves exact local position");
   Require(!std::memcmp(&savedWorld,&sprite->m_WorldMatrix[3],sizeof(VxVector)),"facing preserves exact world position");
   Require(!std::memcmp(&childWorld,&child->m_WorldMatrix,sizeof(VxMatrix)),"facing keeps child world transform");
   if(mode==VXSPRITE3D_BILLBOARD)Require(!std::memcmp(&camera,&sprite->m_WorldMatrix,3*sizeof(VxVector4)),"billboard still faces camera");
   if(mode==VXSPRITE3D_XROTATE)Require(sprite->m_WorldMatrix[0][0]==1&&sprite->m_WorldMatrix[0][1]==0&&sprite->m_WorldMatrix[0][2]==0,"X-facing axis retained");
   if(mode==VXSPRITE3D_YROTATE)Require(sprite->m_WorldMatrix[1][0]==0&&sprite->m_WorldMatrix[1][1]==1&&sprite->m_WorldMatrix[1][2]==0,"Y-facing axis retained");
   if(mode==VXSPRITE3D_ORIENTABLE)Require(!std::memcmp(&oldWorld,&sprite->m_WorldMatrix,sizeof(VxMatrix)),"orientable sprite unchanged");
  }
 }
 Require(CKCloseContext(ctx)==CK_OK,"close");Require(CKShutdown()==CK_OK,"shutdown");std::printf("PASS %u sprite position/orientation checks\n",checks);
}
