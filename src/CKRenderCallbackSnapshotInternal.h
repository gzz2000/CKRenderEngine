#pragma once
class CKObject;
class CKContext;
// Destruction notifications only; no changes to ordinary callback execution.
void CKInvalidateRenderCallbackOwner(CKObject *);
void CKInvalidateRenderCallbackContext(CKContext *);
