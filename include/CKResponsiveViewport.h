#pragma once
#include "CKRenderContext.h"

// Presentation policy only. Camera objects/animations and simulation state stay
// untouched. Browser hosts opt in; native/legacy contexts keep their policy.
extern "C" void CKSetResponsiveViewport(CKRenderContext *context, CKBOOL enabled);

// Returns TRUE only for an opted-in main view. The same authored UI rectangle
// is shared by homogeneous entities, root nodes and screen-relative text.
extern "C" CKBOOL CKGetResponsiveUIRect(CKRenderContext *context, VxRect *rect);
