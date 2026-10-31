// Real renderer API regression for original CK2_3D 0x1005dba1 semantics.
#include "RCK2dEntity.h"
#include "CKGlobals.h"
#include <cstdio>
#include <stdexcept>

static void Require(bool ok,const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static void Equal(const VxRect &a,const VxRect &b,const char *message) {
    Require(a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom,message);
}
int main() {
    try {
        Require(CKStartUp()==CK_OK,"startup");
        CKCLASSREGISTERCID(RCK2dEntity,CKCID_RENDEROBJECT);
        CKBuildClassHierarchyTable();
        CKContext *ctx=nullptr;
        Require(CKCreateContext(&ctx,nullptr,0,0)==CK_OK,"context");
        auto *parent=static_cast<RCK2dEntity *>(ctx->CreateObject(CKCID_2DENTITY));
        auto *child=static_cast<RCK2dEntity *>(ctx->CreateObject(CKCID_2DENTITY));
        Require(parent && child,"entities");
        VxRect parentRect(16,32,272,160),pixels(48,64,144,112),homogeneous(.125f,.25f,.5f,.625f);
        parent->SetRect(parentRect);
        child->SetParent(reinterpret_cast<CK2dEntity *>(parent));
        child->SetRect(pixels);
        for(int cycle=0;cycle<4;++cycle) {
            child->SetHomogeneousCoordinates(TRUE);
            VxRect got;
            Require(child->GetHomogeneousRect(got)==CK_OK,"homogeneous mode");
            Equal(got,homogeneous,"pixel-to-homogeneous conversion");
            Require((child->GetFlags() & CK_2DENTITY_UPDATEHOMOGENEOUSCOORD)!=0,"update flag");
            child->SetHomogeneousCoordinates(TRUE);
            child->GetHomogeneousRect(got);
            Equal(got,homogeneous,"repeated enable must not reconvert");
            child->SetHomogeneousCoordinates(FALSE);
            child->GetRect(got);
            Equal(got,pixels,"homogeneous-to-pixel conversion");
            Require(!child->IsHomogeneousCoordinates(),"disable flag");
        }
        child->SetHomogeneousCoordinates(TRUE);
        VxRect moved(.25f,.125f,.75f,.875f);
        child->SetHomogeneousRect(moved);
        child->SetHomogeneousCoordinates(FALSE);
        VxRect got;child->GetRect(got);
        Equal(got,VxRect(80,48,208,144),"edited homogeneous rectangle conversion");
        CKCloseContext(ctx);CKShutdown();
        std::puts("PASS homogeneous rectangle conversion, repeat toggle, edit and flags");
        return 0;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;
    }
}
