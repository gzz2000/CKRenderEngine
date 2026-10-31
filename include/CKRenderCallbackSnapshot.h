#pragma once

class CKContext;
using CKRenderCallbackSnapshot = void *;

#if defined(_WIN32)
#define CK_CALLBACK_SNAPSHOT_EXPORT extern "C" __declspec(dllexport)
#else
#define CK_CALLBACK_SNAPSHOT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// In-process, same-context snapshots. Call only at a quiescent simulation/render
// boundary. Callback code modules and argument owners must remain alive; the
// caller's semantic snapshots own those lifetimes. Render-owner or manager
// destruction invalidates the handle even if an ID/address is later reused.
// Return CK_OK or a CKERROR. Capture never invokes/reorders callbacks. Restore
// changes only captured owners and preserves callbacks on newly created owners.
// Includes the scene graph's persistent transparent draw order: stable sorting
// retains history when depth comparisons tie, so this is observable render state.
// Membership flags and depth entries are restored together; newly created
// owners retain independent entries for the caller's lifetime reconciliation.
CK_CALLBACK_SNAPSHOT_EXPORT int CKCaptureRenderCallbacks(CKContext *, CKRenderCallbackSnapshot *);
CK_CALLBACK_SNAPSHOT_EXPORT int CKValidateRenderCallbacks(CKContext *, CKRenderCallbackSnapshot);
CK_CALLBACK_SNAPSHOT_EXPORT int CKRestoreRenderCallbacks(CKContext *, CKRenderCallbackSnapshot);
CK_CALLBACK_SNAPSHOT_EXPORT void CKDestroyRenderCallbacks(CKRenderCallbackSnapshot);
