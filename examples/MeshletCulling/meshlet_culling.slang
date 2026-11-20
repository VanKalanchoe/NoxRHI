// MeshletCulling: task + mesh shaders. A task shader culls every meshlet of every instance against the view frustum and the meshlet's
// normal cone, and launches mesh shader groups only for the visible ones; a mesh shader expands a meshlet into vertices and triangles.
// One source for Vulkan and D3D12 -- and the two things the Nox engine's meshlet path needs changed for D3D12:
//   - the meshlet triangle stream is bytes (three local vertex indices per triangle). DXIL has no uint8_t, so the bytes are read out of
//     32-bit words with nox_gpu.slang's LoadByte (identical on Vulkan) instead of through a uint8_t pointer;
//   - the task payload holds 32-bit meshlet indices, not uint8_t offsets.


// All of MeshletCulling's shaders in one module (the default). --split-files loads the task shader from meshlet_task.slang and the mesh /
// fragment shaders from meshlet_mesh.slang instead, each compiled from its own file -- to tell whether keeping task and mesh shaders in
// one Slang file matters on Vulkan.
#include "meshlet_task.slang"
#include "meshlet_mesh.slang"
