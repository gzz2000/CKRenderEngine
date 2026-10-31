# Animation checkpoint state audit

E3146–E3159, 2026-10-31. This covers the native object/master animation
subsystem; it is not an assertion that the whole rendered world can be rewound.

The previous PracticeTools checkpoint stored only animation step. In the same
real game process, changing a channel's target, keys and a master's membership,
length, timing and centering after capture produced 20 failed checks even though
both restores returned success. Exact entity matrices at the moment of restore
cannot repair the next animation evaluation.

## Field ownership

| Implementation | State captured | Treatment |
| --- | --- | --- |
| `RCKAnimation` | Character reference, length, step, cached root, complete flags and frame rate | Copy fields without `GetRootEntity` (which can populate the cache), setters or evaluation. Validate referenced CK lifetimes. |
| `RCKKeyedAnimation` | Ordered children, merged flag, merge factor, cached root animation, centering vector | Prepare replacement list and swap it at commit. Do not recompute a root or propagate merge factors to children. |
| `RCKObjectAnimation` | Keyframe group identity, flags, entity binding, step, merge factor, two merged sources, reserved field, parent keyed animation | Preserve the complete graph, including back references. Do not call `Set3dEntity` or `SetStep` while restoring. |
| `RCK3dEntity::m_ObjectAnimations` | Allocation presence, ordered entries, duplicates/null entries if present | Snapshot independently of forward bindings. Restore the reciprocal list exactly; a new list must not be synthesized by adding channels in enumeration order. |
| `CKKeyframeData` | Five controller slots, length, reference count, owner animation and sharing group | Deep copy each unique group once. Count registered sharers, validate the owner's membership, and reconstruct aliases together. Snapshot copies are ordinary C++ storage, not CK objects. |
| Linear position/rotation/scale/scale-axis controller | Exact type, key count, length and key payloads | Native `Clone` deep-copies the keys and common fields. |
| TCB position/rotation/scale/scale-axis controller | Above, plus optional two-tangents-per-key cache | Public `Clone` invalidates the cache; the snapshot additionally copies its presence and contents without evaluation. Direct key edits can leave a stale but observable cache. |
| Bezier position/scale controller | Keys including computed tangent vectors, cache-computed flag, common fields | `Clone` preserves the key data but clears the flag; the snapshot restores that flag separately. |
| Morph controller | Vertex count, key time, independent position/normal array presence and contents, common fields | `Clone` performs a deep copy, including absent normals and zero-length allocation presence. |

The implementations of every built-in controller's `Clone`, key storage and lazy
cache creation were read before selecting these operations. No `Save`/`Load`
round trip or raw whole-object memory copy is used. Unknown controller factories
or malformed keyframe reference counts reject capture.

## Lifetime and commit boundary

`CKCaptureAnimationState` reads only. It records registration lifetime tokens,
not merely CK IDs or pointer equality. It neither evaluates poses nor changes
controller caches. A failed capture does not replace the caller's old checkpoint.

`CKPrepareAnimationRestore` checks saved identities and allocates all new
controller groups and lists. PracticeTools calls it before deleting abandoned
world objects or applying values. The checkpoint itself is immutable and can
prepare multiple restores.

`CKCommitAnimationRestore` validates lifetimes again and rejects unreconciled new
animation or 3D entity owners before mutation. Then it releases live controller
references, transfers prepared alias groups, swaps entity/master lists and
writes scalar/reference fields. No pose evaluation or allocation occurs in the
mutation phase. A prepared handle can commit once. Destroy it before destroying
its captured source handle.

The caller owns world lifetime reconciliation and exact entity/mesh poses.
PracticeTools commits the animation graph where it formerly called `SetStep`;
its later exact-transform restore remains authoritative. Ordinary saved objects
that were destroyed after capture still cause rejection. This API does not
rehydrate them and does not roll back unrelated world mutations made by the
caller before commit.

## Evidence and remaining boundaries

- E3146: old combination, same actual snapshot API, 27 checks / 20 failures;
  both restores reported success. Process exit 0 and loaded role hashes checked.
- E3152: same fixture binary with only renderer and PracticeTools changed,
  27/27 checks, including the next evaluated centered pose.
- E3154: expanded real-game fixture, 494/494 checks; all 11 controller types,
  lazy caches, morph payloads, sharing ownership transfer, reciprocal list order,
  merging, three overwritten checkpoints and nine repeated restores.
- E3155 HS04 and E3156 fan replay pass respectively 5625/2400 complete GPU
  readbacks and 30,000/6400 repeated states per physical stream, score/collectible
  and RNG gates. External startup differences remain explicitly reported.
- E3159 adds provider-lifecycle rejection checks; E3168 passes 671 checks with
  explicit evaluation-success assertions and bytewise output comparisons.
  E3172 also passes all 671 checks with the final typed-output fixture source.
- E3162–E3165 rerun material88/reference134/capture-failure216/shared-input253
  checks with the same candidate; all pass. These are scoped results, not a
  substitute for whole-world or special author-environment qualification.

`RCKCharacter`'s own transition controller, body-part/exclusive-animation state,
skinning data, animation-related BB private state and arbitrary third-party
extensions have different owners. The character *reference* in a master is
captured here; the character's private runtime is not thereby captured. These
remain independent audit items. Other renderer state, full-world transactional
failure rollback, destroyed-owner rehydration and final TAS/app acceptance are
not established by this subsystem change.
