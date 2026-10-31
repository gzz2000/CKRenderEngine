#include <cstring>
#include "CKContext.h"
#include "CKGlobals.h"
#include "RCK3dEntity.h"
#include "TestTriangleMultiset.h"
#include "VxLegacyMatrix.h"
extern void SetProcessorSpecific_FunctionsPtr();
namespace {
unsigned inversions=0,translations=0;
void Inverse(float *out,const float *) {
    ++inversions;std::memset(out,0,64);out[0]=out[5]=out[10]=out[15]=1;out[12]=123;
}
void Translation(float *out,const float *world) {++translations;out[12]=world[12]+1000;}
void CacheTracksArithmeticMode() {
    CKContext *context=nullptr;
    TestCheck(CKCreateContext(&context,nullptr,0,0)==CK_OK && context,"create context");
    auto *object=new RCK3dEntity(context,"cache test");
    VxMatrix world=VxMatrix::Identity();world[3][0]=2;world[3][1]=3;world[3][2]=4;
    object->SetWorldMatrix(world);
    TestCheck(object->GetInverseWorldMatrix()[3][0]==-2,"initial native inverse");
    const VxLegacyMatrixBackend backend{Inverse,Translation};
    auto epoch=VxGetLegacyMatrixGeneration();VxSetLegacyMatrixBackend(&backend);
    TestCheck(VxGetLegacyMatrixGeneration()!=epoch,"switch increments generation");
    TestCheck(object->GetInverseWorldMatrix()[3][0]==123 && inversions==1,"reject native cache");
    object->GetInverseWorldMatrix();
    TestCheck(inversions==1,"reuse same-mode cache");
    epoch=VxGetLegacyMatrixGeneration();VxSetLegacyMatrixBackend(&backend);
    TestCheck(VxGetLegacyMatrixGeneration()==epoch,"idempotent mode selection");
    VxVector next(5,6,7);object->SetPosition(&next);
    TestCheck(translations==1 && object->GetInverseWorldMatrix()[3][0]==1005 && inversions==1,"incremental translation uses provider");
    VxSetLegacyMatrixBackend(nullptr);
    TestCheck(object->GetInverseWorldMatrix()[3][0]==-5,"return to native recomputes cache");
    VxSetLegacyMatrixBackend(&backend);
    next.x=8;object->SetPosition(&next);
    TestCheck(translations==1,"do not incrementally update previous-mode inverse");
    TestCheck(object->GetInverseWorldMatrix()[3][0]==123 && inversions==2,"position change after switch recomputes");
    VxSetLegacyMatrixBackend(nullptr);
    next.x=9;object->SetPosition(&next);
    TestCheck(object->GetInverseWorldMatrix()[3][0]==-9,"provider removal before position change");
    CKCloseContext(context);
}
}
int main() {
    SetProcessorSpecific_FunctionsPtr();TestCheck(CKStartUp()==CK_OK,"startup");
    CKCLASSREGISTERCID(RCK3dEntity,CKCID_RENDEROBJECT);CKBuildClassHierarchyTable();
    TestFramework tests;tests.Run("Inverse cache tracks arithmetic mode",CacheTracksArithmeticMode);
    VxSetLegacyMatrixBackend(nullptr);
    const int result=tests.ExitCode();TestCheck(CKShutdown()==CK_OK,"shutdown");return result;
}
