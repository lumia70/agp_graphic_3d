/*
 * gaussian_splat_data.cpp — Storage for shared gaussian splatting data
 * Compiled into liblumeapp.so with C linkage for cross-.so access
 */
#include "gaussian_splat_data.h"

extern "C" {
__attribute__((visibility("default"))) const GpuGaussianData* g_gaussianSplatData = nullptr;
__attribute__((visibility("default"))) uint32_t g_gaussianSplatCount = 0;
__attribute__((visibility("default"))) uint32_t g_gaussianSplatCapacity = 0;
__attribute__((visibility("default"))) uint32_t g_gaussianSplatGeneration = 0;
__attribute__((visibility("default"))) uint32_t g_gaussianSplatUploadStart = 0;
__attribute__((visibility("default"))) uint32_t g_gaussianSplatUploadedGeneration = 0;
__attribute__((visibility("default"))) const float* g_gaussianShData = nullptr;
__attribute__((visibility("default"))) GaussianCameraData g_gaussianCameraData = {};
__attribute__((visibility("default"))) float g_gaussianBackground[4] = { 0.071f, 0.047f, 0.024f, 1.0f };
__attribute__((visibility("default"))) bool g_gaussianDataReady = false;
__attribute__((visibility("default"))) bool g_gaussianCameraReady = false;
}
