# Yoshi: invalid workgroup inputs in a vertex shader (2026-11-14)

The `9a` diagnostic run aborted in ACO while compiling graphics pipeline
`35646e8f72a099be`. Its vertex shader is `69a70c16e0d4c457`, module hash
`d0e607e0bfc6cd06`; the fragment shader is `153888f2b401f4f3`.
The captured vertex SPIR-V declares and reads WorkgroupId and LocalInvocationId.
With scalar block layout enabled, spirv-val rejects it with
VUID-WorkgroupId-WorkgroupId-04422. Both captured fragment variants paired with
that vertex module reproduce `Unimplemented intrinsic ... load_base_workgroup_id`
and SIGABRT in the offline compiler built from the actual PS5 Mesa source.

RADV's system-value pass introduces base_workgroup_id for WorkgroupId. Its next
compute-system-value pass returns immediately for non-workgroup stages, so the
invalid vertex module leaves an intrinsic ACO does not accept. A valid compute
control using the same builtin compiles successfully and has no remaining
base_workgroup_id. The emitter now rejects these inputs for stages other than
Compute, Task and Mesh before creating a module. This prevents this invalid
pipeline from entering the driver; it does not repair the missing draw.

The exception follows the existing pipeline failure path. Synchronous creation
returns nullptr; deferred emission logs the pipeline hash and PipelineBuild marks
the job failed without submitting the driver stage. Fragment ClipDistance writes
already throw in DefineOutputs, so that separate failure is unchanged.

## Remaining cause

The cached environment identifies this program as VertexB (Stage value 0), not
VertexA. Its lowered instructions contain S2R CTAID_X/Y (0x25/0x26) and TID_X/Y
(0x21/0x22). These numbers agree with Mesa Nouveau NAK's special-register table;
there is no evidence for replacing them with zero or another register. Why these
instructions became reachable in a vertex program remains unresolved: native
program capture/control flow and guest stage assignment still need investigation.
The earlier offline replay could not emit this vertex program because a recorded
texture pixel format at handle 0x128001a5 was missing. That replay gap is now
covered by the actual console module for the driver reproduction, not by guessed
texture data.

## Upstream check

Read the actual diffs for Eglantine PRs
[762](https://github.com/Djihads80/Eglantine/pull/762) and
[772](https://github.com/Djihads80/Eglantine/pull/772). The former retains S2R
special-register numbering; the latter adds task/mesh and texture diagnostics.
Neither adds this stage validation. The available upstream/nx2 source likewise
emits these builtins without checking the shader stage.
Official Mesa main's `nir_lower_compute_system_values` still has the same
workgroup-stage guard, and RADV still passes zero-initialized options that lower
base_workgroup_id to zero for valid compute stages. There is no matching RADV
lowering fix to port. GitHub issue search was unavailable (HTTP 422), so this is
not a claim that every upstream issue was searched successfully.

## Offline validation

`src/tests/video_core/spirv_builtin_capabilities.cpp` exercises actual
EmitContext construction for all nine stages and all four combinations of the
two builtin-use flags: invalid stages throw, valid workgroup stages and shaders
without these inputs continue. The focused file passes 59 assertions in four
test cases, including its existing capability tests.

Private evidence and replay commands are under the workspace's
`runs/yoshi_aco_fix_20261114/base-workgroup/` and
`runs/yoshi_immediate_crash_20261114/shader-debug/offline/`.
No captured game shader is included in this source commit. No console test is
claimed for this change.
