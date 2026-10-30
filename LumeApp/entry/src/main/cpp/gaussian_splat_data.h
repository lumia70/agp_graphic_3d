/*
 * gaussian_splat_data.h — Shared data between app and RenderNode
 *
 * Both liblumeapp.so and LumeRender use this header.
 * The actual storage is defined in gaussian_splat_data.cpp (compiled into liblumeapp.so),
 * and the RenderNode in LumeRender accesses it via extern declarations.
 */
#pragma once

#include <cstdint>

struct GpuGaussianData {
    float px, py, pz, pad0;
    float sx, sy, sz, opacity;
    float qw, qx, qy, qz;
    float cr, cg, cb, ca;
};

struct GaussianCameraData {
    float view[16];
    float proj[16];
    float viewport[4];
    float cameraPos[4];
    float shScale[4];   // x = SH degree 1 scale, y = degree 2-3 scale
    float clip[4];      // x = near plane, y = far plane (sorting/cull range)
};

// Shared global state
// Defined with C linkage so they can be found via dlsym across .so boundaries
#ifdef __cplusplus
extern "C" {
#endif

extern __attribute__((visibility("default"))) const GpuGaussianData* g_gaussianSplatData;
extern __attribute__((visibility("default"))) uint32_t g_gaussianSplatCount;
// Capacity of the array behind g_gaussianSplatData (LOD re-assembly can change
// the active count without reallocating).
extern __attribute__((visibility("default"))) uint32_t g_gaussianSplatCapacity;
// Bumped by the app whenever g_gaussianSplatData/count change. The render node
// re-uploads when it sees a new value and echoes it back after the upload.
extern __attribute__((visibility("default"))) uint32_t g_gaussianSplatGeneration;
// Index where the data for this generation starts. 0 means "the whole range was
// rewritten" (the node then switches to its alternate SSBO); > 0 means the
// existing prefix is unchanged and only [start, count) needs uploading.
extern __attribute__((visibility("default"))) uint32_t g_gaussianSplatUploadStart;
extern __attribute__((visibility("default"))) uint32_t g_gaussianSplatUploadedGeneration;
extern __attribute__((visibility("default"))) const float* g_gaussianShData;
extern __attribute__((visibility("default"))) GaussianCameraData g_gaussianCameraData;
extern __attribute__((visibility("default"))) bool g_gaussianDataReady;
extern __attribute__((visibility("default"))) bool g_gaussianCameraReady;

#ifdef __cplusplus
}
#endif
