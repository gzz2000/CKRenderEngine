#pragma once
class CKContext;
#if defined(_WIN32)
#define CK_ANIMATION_SNAPSHOT_EXPORT extern "C" __declspec(dllexport)
#else
#define CK_ANIMATION_SNAPSHOT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
// Engine-thread, quiescent, same-context checkpoint of animation runtime data.
// Captures controllers (including lazy caches), sharing/ownership, bindings,
// ordered channel/entity lists, timing, merging and cached root/centering.
// Does not evaluate poses or change transforms. The world snapshot owns those.
// Ordinary objects must remain alive. Caller must remove post-capture entities
// and animations before Commit; Prepare can precede that reconciliation.
// Prepare allocates all replacements without mutation. Commit validates again,
// then transfers prepared storage without allocation or animation evaluation.
// A prepared handle is single-use; the captured handle is reusable. Destroy
// prepared handles before destroying their source snapshot.
CK_ANIMATION_SNAPSHOT_EXPORT int CKCaptureAnimationState(CKContext *, void **);
CK_ANIMATION_SNAPSHOT_EXPORT int CKPrepareAnimationRestore(CKContext *, void *, void **);
CK_ANIMATION_SNAPSHOT_EXPORT int CKCommitAnimationRestore(CKContext *, void *);
CK_ANIMATION_SNAPSHOT_EXPORT void CKDestroyAnimationRestore(void *);
CK_ANIMATION_SNAPSHOT_EXPORT void CKDestroyAnimationState(void *);
