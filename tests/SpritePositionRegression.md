# Camera-facing sprite position invariant

A render-only facing update must preserve both world and parent-relative sprite
translation bit-for-bit. `RCKSprite3D::UpdateOrientation` updates orientation via
`WorldMatrixChanged(TRUE, TRUE)`. That function computes local from inverse(parent)
and world, which can round a previously valid local translation. TT Extra reads
that local translation on the next simulation tick; rendering therefore changed
collectible orbits, even in the null rasterizer.

The facing update now preserves its original local translation while retaining
orientation updates, child world transforms and render invalidation. This applies
to billboards and the X/Y-axis facing modes. Orientable sprites keep their previous
behavior. It does not suppress rendering or change the physics timestep.

`test_sprite_position.cpp` uses the real renderer classes without a window or GPU.
It creates a translated parent and a camera-facing sprite with a child, switches
camera orientation repeatedly, and checks exact local/world position, child world
transform, billboard orientation and X/Y-axis constraints. The old renderer fails
the first local-position check; the fixed renderer passes all 517 checks.

Runtime evidence is recorded in the enclosing workspace experiment log:

- E2726 versus E2722: rendering every simulation step versus every 64 steps first
  changed TT Extra output at sample 2013, while captured player physics still
  matched the original Windows reference.
- E2732: a read-only pre/post-render observer sees world translation unchanged,
  but local translation changed at that boundary. Its complete 763,926 observed
  Extra rows match the uninstrumented batch-1 run (E2736).
- E2737: the same observer with the fix reports zero position changes, with no
  diagnostic rollback enabled.
- E2742/E2743: complete Extra observations are equal for batches 1, 64 and 256.
  Old and new visual coordinates need not match (E2744), because the old result
  incorporated rendering-dependent rounding. This is not proof of full original
  renderer fidelity, all TAS author environments, or complete world snapshots.

The native BML+ `RenderHookPosix::SkipNextRender` is currently a no-op. Consequently
its TAS skip-render setting did not eliminate this dependency; that separate
feature still requires an implementation and validation.
