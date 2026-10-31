#pragma once
#include "CKRenderContext.h"

// Presentation policy only. Camera objects/animations and simulation state stay
// untouched. Browser hosts opt in; native/legacy contexts keep their policy.
extern "C" void CKSetResponsiveViewport(CKRenderContext *context, CKBOOL enabled);
