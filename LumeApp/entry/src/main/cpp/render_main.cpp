/*
 * Lume 3D engine rendering demo — full engine init via XComponent.
 *
 * Phase 3: Initialize the Lume engine with Vulkan backend, create ECS,
 * load a 3D Gaussian Splatting model, set up camera + light, and render via
 * TickFrame.
 */
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <dlfcn.h>
#include <time.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <native_window/external_window.h>
#include <rawfile/raw_file_manager.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include <rawfile/raw_dir.h>
#include <rawfile/raw_file.h>

#include "splat_loader.h"
#include "sog_loader.h"
#include "lod_streamer.h"

// LumeGS render path (isolated from the legacy custom splat pipeline). For now
// it only loads the model through the plugin's ECS components so the loading
// pipeline can be validated; the render node is wired next.
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <plugin_gs/ecs/components/gs_component.h>
#include <plugin_gs/ecs/components/gs_configuration_component.h>
#include <plugin_gs/ecs/components/gs_tiling_component.h>
#include <plugin_gs/ecs/systems/gs_system.h>
#include <plugin_gs/ecs/systems/intf_gs_tiling_system.h>
#include "gaussian_splat_data.h"

// --- LumeBase ---
#include <base/containers/array_view.h>
#include <base/containers/refcnt_ptr.h>
#include <base/containers/string.h>
#include <base/containers/unique_ptr.h>
#include <base/math/mathf.h>
#include <base/math/matrix.h>
#include <base/math/quaternion.h>
#include <base/math/vector.h>
#include <base/util/uid.h>

// --- LumeEngine ---
#include <core/ecs/intf_ecs.h>
#include <core/ecs/intf_system_graph_loader.h>
#include <core/engine_info.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file_manager.h>
#include <core/os/platform_create_info.h>
#include <core/plugin/intf_class_factory.h>
#include <core/plugin/intf_class_register.h>
#include <core/plugin/intf_plugin_register.h>

// --- LumeRender ---
#include <render/device/intf_device.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/implementation_uids.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/environment_component.h>

#include <render/intf_render_context.h>
#include <render/intf_renderer.h>

// --- Lume_3D ---
#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/layer_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/render/intf_render_data_store_default_material.h>
#include <3d/implementation_uids.h>
#include <3d/intf_graphics_context.h>
#include <3d/util/intf_scene_util.h>

// ---------------------------------------------------------------------------
// Log helpers
// ---------------------------------------------------------------------------
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x4C4D /* "LM" */
#undef LOG_TAG
#define LOG_TAG "LumeApp"
#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define LOGW(...) OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Engine state (all access on XComponent callback thread)
// ---------------------------------------------------------------------------
namespace {

// Rawfile base path inside the OHOS HAP sandbox.
constexpr const char* RAWFILE_BASE = "/data/storage/el1/bundle/entry/resources/rawfile/";

struct LumeState {
    OHNativeWindow* nativeWindow = nullptr;
    uint64_t width = 0;
    uint64_t height = 0;
    bool ready = false;

    // Engine objects — ordered by destruction dependency (reverse init order).
    CORE_NS::IEcs::Ptr ecs;
    CORE3D_NS::IGraphicsContext::Ptr graphicsContext;
    RENDER_NS::IRenderContext::Ptr renderContext;
    CORE_NS::IEngine::Ptr engine;

    // Non-owning pointers (owned by contexts above).
    RENDER_NS::IDevice* device = nullptr;
    RENDER_NS::IRenderer* renderer = nullptr;
    RENDER_NS::RenderHandleReference swapchainHandle;

    // Keep importer alive to prevent EntityReference cleanup from destroying imported entities.

    // 3DGS splat data
    SplatData splatData;
    bool splatLoaded = false;
    RENDER_NS::RenderHandleReference splatRngHandle;  // RNG for gaussian splatting

    // Frame counter for logging.
    uint64_t frameCount = 0;
};

LumeState g_lume;
bool g_pluginRegistryCreated = false;
napi_env g_napiEnv = nullptr;
NativeResourceManager* g_resMan = nullptr;
std::string g_filesDir;  // app sandbox writable directory

// One-entry cache for single-model files. The common demo flow is
// Doll (default) -> City LOD -> Doll; re-decoding 400k+ gaussians on the
// render thread made the switch frame ~0.5s and starved touch input.
static std::string g_cachedSplatPath;
static SplatData g_cachedSplatData;
static std::string g_splatDataInUsePath;

// In-flight single-model decode generation. Release/cache hits bump it so a
// worker that finishes late cannot overwrite a newer model.
static uint32_t g_splatDecodeGeneration = 0;
static bool g_splatDecodeQueued = false;

// ---------------------------------------------------------------------------
// Orbit Camera for 3DGS scene exploration
// Free-tumble arcball: orientation accumulated as world-space basis vectors
// (right/up/back) with Rodrigues rotations — no gimbal lock, no pitch clamp.
// ---------------------------------------------------------------------------
struct OrbitCamera {
    // Camera basis in world space (orthonormal)
    float rightX = 1.f, rightY = 0.f, rightZ = 0.f;
    float upX = 0.f, upY = 1.f, upZ = 0.f;
    float backX = 0.f, backY = 0.f, backZ = 1.f;

    float fovYDeg = 60.0f;  // vertical field of view (per-model)
    float shDeg1 = 0.0f;    // SH degree-1 weight (per-model)
    float shDeg23 = 0.0f;   // SH degree 2-3 weight (per-model)
    float distance = 1.2f;  // distance from target
    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
    float nearPlane = 0.05f;
    float farPlane = 2000.0f;
    // Smallest orbit distance: below this the camera would sit inside the
    // geometry and the near cull could hide the whole scene.
    float minDistance = 0.1f;
    // First-person ("walk") mode for large scenes: the eye IS the target, so a
    // drag rotates in place and two-finger gestures move through the scene.
    bool firstPerson = false;
    float fpMoveScale = 1.0f;  // world units per pixel of two-finger drag
    float fpBackoff = 0.0f;    // orbit distance restored when leaving first person
    // Scene pivot used when switching back to orbit (otherwise the target is
    // derived from the view direction, which can point at the sky).
    float pivotX = 0.0f, pivotY = 0.0f, pivotZ = 0.0f;
    bool hasPivot = false;
    // Roaming bounds: keep the eye inside the scene (expanded) so a fast walk
    // cannot leave the model behind and show an empty screen.
    float boundsMin[3] = { -1e30f, -1e30f, -1e30f };
    float boundsMax[3] = { 1e30f, 1e30f, 1e30f };
    bool hasBounds = false;

    // Touch state
    float lastX = 0.0f, lastY = 0.0f;
    float lastDist = 0.0f;  // pinch distance
    int activePointers = 0;
    bool dragging = false;
    bool pinching = false;

    // Double-tap shortcut: toggle between far framing and the model centre
    double lastTapTimeMs = 0.0;
    float lastTapX = 0.0f, lastTapY = 0.0f;
    float farDistance = 100.0f;    // full-scene framing (double-tap target)
    float insideDistance = 4.0f;   // near the content centre (startup view)
    bool insideView = true;

    // Rotate vector v around unit axis k by angle (Rodrigues)
    static void RotateVec(float& vx, float& vy, float& vz,
        float kx, float ky, float kz, float angle) {
        const float c = cosf(angle), s = sinf(angle);
        const float kxv = ky * vz - kz * vy;
        const float kyv = kz * vx - kx * vz;
        const float kzv = kx * vy - ky * vx;
        const float kdotv = kx * vx + ky * vy + kz * vz;
        const float nx = vx * c + kxv * s + kx * kdotv * (1.f - c);
        const float ny = vy * c + kyv * s + ky * kdotv * (1.f - c);
        const float nz = vz * c + kzv * s + kz * kdotv * (1.f - c);
        vx = nx; vy = ny; vz = nz;
    }

    // Tumble: rotate around camera up (yaw feel) and camera right (pitch feel)
    void Tumble(float yawDelta, float pitchDelta) {
        if (yawDelta != 0.f) {
            // rotate right & back around up
            RotateVec(rightX, rightY, rightZ, upX, upY, upZ, yawDelta);
            RotateVec(backX, backY, backZ, upX, upY, upZ, yawDelta);
        }
        if (pitchDelta != 0.f) {
            // rotate up & back around right
            RotateVec(upX, upY, upZ, rightX, rightY, rightZ, pitchDelta);
            RotateVec(backX, backY, backZ, rightX, rightY, rightZ, pitchDelta);
        }
    }

    void ComputeViewMatrix(float* out) const {
        // Camera looks along -back; eye = target + back * distance
        const float ex = targetX + backX * distance;
        const float ey = targetY + backY * distance;
        const float ez = targetZ + backZ * distance;
        // Column-major view matrix (world→view): rows = right, up, back
        // (view space: camera at origin looking down -Z, so in-front points have z < 0)
        out[ 0]=rightX;  out[ 1]=upX;   out[ 2]=backX;  out[ 3]=0.f;
        out[ 4]=rightY;  out[ 5]=upY;   out[ 6]=backY;  out[ 7]=0.f;
        out[ 8]=rightZ;  out[ 9]=upZ;   out[10]=backZ;  out[11]=0.f;
        out[12]=-(rightX*ex + rightY*ey + rightZ*ez);
        out[13]=-(upX*ex + upY*ey + upZ*ez);
        out[14]=-(backX*ex + backY*ey + backZ*ez);
        out[15]=1.f;
    }

    void ComputeProjMatrix(float* out, float aspect) const {
        float fovRad = fovYDeg * 3.14159265f / 180.f;
        float f = 1.f / tanf(fovRad * 0.5f);
        float zn = nearPlane, zf = farPlane;
        memset(out, 0, 16 * sizeof(float));
        out[0]  = f / aspect;
        out[5]  = -f;  // Vulkan Y-flip
        out[10] = zf / (zn - zf);
        out[11] = -1.f;
        out[14] = (zn * zf) / (zn - zf);
    }

    void GetPosition(float& x, float& y, float& z) const {
        x = targetX + backX * distance;
        y = targetY + backY * distance;
        z = targetZ + backZ * distance;
    }

    // Pan target along camera right/up so content follows the finger
    void ClampTarget() {
        if (!hasBounds) {
            return;
        }
        targetX = targetX < boundsMin[0] ? boundsMin[0] : (targetX > boundsMax[0] ? boundsMax[0] : targetX);
        targetY = targetY < boundsMin[1] ? boundsMin[1] : (targetY > boundsMax[1] ? boundsMax[1] : targetY);
        targetZ = targetZ < boundsMin[2] ? boundsMin[2] : (targetZ > boundsMax[2] ? boundsMax[2] : targetZ);
    }

    // Walk mode movement: dx strafes, dy dollies along the view direction.
    void Walk(float dxScreen, float dyScreen, float scale) {
        targetX += (rightX * dxScreen - backX * dyScreen) * scale;
        targetY += (rightY * dxScreen - backY * dyScreen) * scale;
        targetZ += (rightZ * dxScreen - backZ * dyScreen) * scale;
        ClampTarget();
    }

    // Dolly along the view direction (positive = forward).
    void Dolly(float forward) {
        targetX -= backX * forward;
        targetY -= backY * forward;
        targetZ -= backZ * forward;
        ClampTarget();
    }

    void Pan(float dxScreen, float dyScreen, float scale) {
        targetX -= (rightX * dxScreen - upX * dyScreen) * scale;
        targetY -= (rightY * dxScreen - upY * dyScreen) * scale;
        targetZ -= (rightZ * dxScreen - upZ * dyScreen) * scale;
    }

    void LogState(const char* when) const {
        LOGI("ORBITCAM[%{public}s]: right=(%{public}.6f,%{public}.6f,%{public}.6f) up=(%{public}.6f,%{public}.6f,%{public}.6f) back=(%{public}.6f,%{public}.6f,%{public}.6f) dist=%{public}.6f target=(%{public}.6f,%{public}.6f,%{public}.6f)",
            when,
            rightX, rightY, rightZ,
            upX, upY, upZ,
            backX, backY, backZ,
            distance,
            targetX, targetY, targetZ);
    }

    void UpdateSharedCameraData(uint32_t width, uint32_t height) {
        float aspect = (float)width / (float)height;
        ComputeViewMatrix(g_gaussianCameraData.view);
        ComputeProjMatrix(g_gaussianCameraData.proj, aspect);
        g_gaussianCameraData.viewport[0] = 0.f;
        g_gaussianCameraData.viewport[1] = 0.f;
        g_gaussianCameraData.viewport[2] = (float)width;
        g_gaussianCameraData.viewport[3] = (float)height;
        float cx, cy, cz;
        GetPosition(cx, cy, cz);
        g_gaussianCameraData.cameraPos[0] = cx;
        g_gaussianCameraData.cameraPos[1] = cy;
        g_gaussianCameraData.cameraPos[2] = cz;
        g_gaussianCameraData.cameraPos[3] = 1.f;
        g_gaussianCameraData.shScale[0] = shDeg1;
        g_gaussianCameraData.shScale[1] = shDeg23;
        g_gaussianCameraData.shScale[2] = 0.f;
        g_gaussianCameraData.shScale[3] = 0.f;
        g_gaussianCameraData.clip[0] = nearPlane;
        g_gaussianCameraData.clip[1] = farPlane;
        g_gaussianCameraData.clip[2] = 0.f;
        g_gaussianCameraData.clip[3] = 0.f;
        g_gaussianCameraReady = true;
    }
};
OrbitCamera g_orbitCam;

// Render-on-demand state: only render when the view changed or shortly after interaction.
static uint32_t g_forceRenderFrames = 30;  // render this many frames after startup/touch
static float g_lastRenderedView[16] = {};

// ---------------------------------------------------------------------------
// Rawfile extraction — copy HAP rawfile assets to app sandbox for file:// access
// ---------------------------------------------------------------------------

static void MkdirRecursive(const std::string& path)
{
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        mkdir(path.substr(0, pos).c_str(), 0755);
    }
    mkdir(path.c_str(), 0755);
}

static bool ExtractRawDir(NativeResourceManager* resMgr, const std::string& rawDir, const std::string& destDir)
{
    if (!resMgr) return false;
    MkdirRecursive(destDir);

    RawDir* dir = OH_ResourceManager_OpenRawDir(resMgr, rawDir.c_str());
    if (!dir) {
        LOGW("Cannot open rawdir: %{public}s", rawDir.c_str());
        return false;
    }

    int count = OH_ResourceManager_GetRawFileCount(dir);
    int extracted = 0;
    for (int i = 0; i < count; i++) {
        const char* name = OH_ResourceManager_GetRawFileName(dir, i);
        if (!name) continue;

        std::string rawPath = rawDir.empty() ? std::string(name) : rawDir + "/" + name;
        std::string destPath = destDir + "/" + name;

        // Try as file first
        RawFile* file = OH_ResourceManager_OpenRawFile(resMgr, rawPath.c_str());
        if (file) {
            long size = OH_ResourceManager_GetRawFileSize(file);
            if (size > 0) {
                std::vector<uint8_t> buf(size);
                OH_ResourceManager_ReadRawFile(file, buf.data(), size);
                OH_ResourceManager_CloseRawFile(file);

                FILE* out = fopen(destPath.c_str(), "wb");
                if (out) {
                    fwrite(buf.data(), 1, size, out);
                    fclose(out);
                    extracted++;
                }
            } else {
                OH_ResourceManager_CloseRawFile(file);
                // Size 0 might be a directory — try recursion
                ExtractRawDir(resMgr, rawPath, destPath);
            }
        } else {
            // Not a file — try as subdirectory
            ExtractRawDir(resMgr, rawPath, destPath);
        }
    }
    OH_ResourceManager_CloseRawDir(dir);
    LOGI("Extracted %{public}d files from rawfile/%{public}s", extracted, rawDir.c_str());
    return true;
}

static std::string ExtractShadersToFilesDir()
{
    if (!g_resMan || g_filesDir.empty()) {
        LOGE("Cannot extract shaders: resMan=%{public}p filesDir=%{public}s",
            g_resMan, g_filesDir.c_str());
        return "";
    }

    std::string shaderBase = g_filesDir + "/shaders";

    // Check if already extracted (skip re-extraction for speed)
    struct stat st;
    // Always extract model assets (they might be updated independently of shaders)
    std::string assetsBase = g_filesDir + "/assets";
    ExtractRawDir(g_resMan, "assets", assetsBase);

    // Always extract splat shaders (may have been added/updated after initial extraction)
    ExtractRawDir(g_resMan, "shaders/splat", shaderBase + "/splat");

    if (stat((shaderBase + "/render/shaders").c_str(), &st) == 0) {
        LOGI("Shaders already extracted at %{public}s (splat shaders refreshed)", shaderBase.c_str());
        return shaderBase;
    }

    LOGI("Extracting shaders from rawfile to %{public}s ...", shaderBase.c_str());
    ExtractRawDir(g_resMan, "shaders/render", shaderBase + "/render");
    ExtractRawDir(g_resMan, "shaders/3d", shaderBase + "/3d");
    ExtractRawDir(g_resMan, "shaders/splat", shaderBase + "/splat");

    LOGI("Shader extraction complete");
    return shaderBase;
}

// ---------------------------------------------------------------------------
// Engine initialization helpers
// ---------------------------------------------------------------------------

// Build the base filesystem path as a file:// URI for the rawfile directory.
BASE_NS::string GetRawfileUri()
{
    return BASE_NS::string("file://") + RAWFILE_BASE;
}

bool InitPluginRegistry()
{
    if (g_pluginRegistryCreated) {
        LOGI("Plugin registry already created");
        return true;
    }

    // Load plugin .so files via dlopen BEFORE creating the registry.
    // They must NOT be in liblumeapp.so's NEEDED deps, otherwise the linker
    // loads them too early (before LumeEngine globals are initialized) and
    // the constructor-based RegisterStaticPlugin writes to uninitialized memory.
    void* hMeta = dlopen("libLumeMeta.so", RTLD_NOW | RTLD_GLOBAL);
    LOGI("dlopen libLumeMeta.so: %{public}s", hMeta ? "OK" : dlerror());
    void* hRender = dlopen("libLumeRender.so", RTLD_NOW | RTLD_GLOBAL);
    LOGI("dlopen libLumeRender.so: %{public}s", hRender ? "OK" : dlerror());
    void* h3D = dlopen("libLume_3D.so", RTLD_NOW | RTLD_GLOBAL);
    LOGI("dlopen libLume_3D.so: %{public}s", h3D ? "OK" : dlerror());

    LOGI("Creating plugin registry...");
    CORE_NS::PlatformCreateInfo platformCI {};
    platformCI.jsEnv = g_napiEnv;
    platformCI.jsObject = nullptr;
    platformCI.resMan = g_resMan;
    platformCI.appPluginPath = RAWFILE_BASE;

    CORE_NS::CreatePluginRegistry(platformCI);
    g_pluginRegistryCreated = true;
    LOGI("Plugin registry created OK");

    return true;
}

bool InitEngine()
{
    LOGI("Creating engine...");

    // Diagnostic: step-by-step verify each call
    LOGI("Step 1: GetPluginRegister...");
    CORE_NS::IPluginRegister& pluginReg = CORE_NS::GetPluginRegister();
    LOGI("Step 1 OK");

    LOGI("Step 2: GetClassRegister...");
    CORE_NS::IClassRegister& classReg = pluginReg.GetClassRegister();
    LOGI("Step 2 OK");

    LOGI("Step 3: GetInstance(UID_ENGINE_FACTORY)...");
    auto* factoryIntf = classReg.GetInstance(CORE_NS::UID_ENGINE_FACTORY);
    LOGI("Step 3: factoryIntf = 0x%{public}lx", (unsigned long)(uintptr_t)factoryIntf);

    auto* factory = static_cast<CORE_NS::IEngineFactory*>(factoryIntf);

    if (!factory) {
        LOGE("Failed to get IEngineFactory");
        return false;
    }
    LOGI("IEngineFactory obtained");

    CORE_NS::EngineCreateInfo engineCI {};
    engineCI.platformCreateInfo.jsEnv = g_napiEnv;
    engineCI.platformCreateInfo.jsObject = nullptr;
    engineCI.platformCreateInfo.resMan = g_resMan;
    engineCI.platformCreateInfo.appPluginPath = RAWFILE_BASE;
    engineCI.applicationVersion.name = "LumeApp";
    engineCI.applicationVersion.versionMajor = 0;
    engineCI.applicationVersion.versionMinor = 1;
    engineCI.applicationVersion.versionPatch = 0;
    if (!factory) {
        LOGE("Failed to get IEngineFactory");
        return false;
    }
    LOGI("IEngineFactory obtained");

    g_lume.engine = factory->Create(engineCI);
    if (!g_lume.engine) {
        LOGE("IEngineFactory::Create returned null");
        return false;
    }
    LOGI("Engine instance created");

    // Extract shaders from HAP rawfile to app sandbox (file:// accessible)
    std::string shaderBase = ExtractShadersToFilesDir();
    if (shaderBase.empty()) {
        LOGE("Shader extraction failed");
        return false;
    }

    // Register filesystem paths using file:// protocol pointing at extracted files
    auto& fm = g_lume.engine->GetFileManager();
    const BASE_NS::string renderUri = BASE_NS::string("file://") + shaderBase.c_str() + "/render/";
    const BASE_NS::string tdUri = BASE_NS::string("file://") + shaderBase.c_str() + "/3d/";
    const BASE_NS::string appUri = BASE_NS::string("file://") + g_filesDir.c_str() + "/";

    fm.RegisterPath("rofsRndr", renderUri, false);
    fm.RegisterPath("rofs3D", tdUri, false);

    // Root protocol paths — 3d:// and render:// point to the root of 3d/render asset dirs
    fm.RegisterPath("3d", tdUri, false);
    fm.RegisterPath("render", renderUri, false);

    // Render paths
    fm.RegisterPath("rendershaders", renderUri + "shaders/", false);
    fm.RegisterPath("rendershaderstates", renderUri + "shaderstates/", false);
    fm.RegisterPath("rendervertexinputdeclarations", renderUri + "vertexinputdeclarations/", false);
    fm.RegisterPath("renderpipelinelayouts", renderUri + "pipelinelayouts/", false);
    fm.RegisterPath("renderrenderdataconfigurations", renderUri + "renderdataconfigurations/", false);
    fm.RegisterPath("renderrendernodegraphs", renderUri + "rendernodegraphs/", false);

    // 3D paths
    fm.RegisterPath("3dshaders", tdUri + "shaders/", false);
    fm.RegisterPath("3dshaderstates", tdUri + "shaderstates/", false);
    fm.RegisterPath("3dvertexinputdeclarations", tdUri + "vertexinputdeclarations/", false);
    fm.RegisterPath("3dpipelinelayouts", tdUri + "pipelinelayouts/", false);
    fm.RegisterPath("3drenderdataconfigurations", tdUri + "renderdataconfigurations/", false);
    fm.RegisterPath("3drendernodegraphs", tdUri + "rendernodegraphs/", false);

    // Splat shader paths
    const BASE_NS::string splatUri = BASE_NS::string("file://") + shaderBase.c_str() + "/splat/";
    fm.RegisterPath("splatshaders", splatUri, false);

    // App assets
    fm.RegisterPath("app", appUri, true);

    LOGI("All shader paths registered (file:// backed)");

    // CRITICAL: Must call LoadPlugins on the PluginRegistry to trigger
    // GatherStaticPlugins → RegisterPlugin → registerInterfaces.
    LOGI("Loading plugins via PluginRegistry...");
    CORE_NS::GetPluginRegister().LoadPlugins({});
    auto loadedPlugins = CORE_NS::GetPluginRegister().GetPlugins();
    LOGI("PluginRegistry::LoadPlugins done — %{public}zu plugins loaded", loadedPlugins.size());
    for (size_t i = 0; i < loadedPlugins.size(); ++i) {
        if (loadedPlugins[i]) {
            LOGI("  Plugin[%{public}zu]: %{public}s", i, loadedPlugins[i]->name);
        }
    }

    // Check registered IEnginePlugin type infos before engine init
    auto enginePluginInfos = CORE_NS::GetPluginRegister().GetTypeInfos(CORE_NS::IEnginePlugin::UID);
    LOGI("IEnginePlugin type infos before Init: %{public}zu", enginePluginInfos.size());

    g_lume.engine->Init();

    // After Init, check engine plugins
    enginePluginInfos = CORE_NS::GetPluginRegister().GetTypeInfos(CORE_NS::IEnginePlugin::UID);
    LOGI("IEnginePlugin type infos after Init: %{public}zu", enginePluginInfos.size());

    // Check what interfaces engine now exposes
    auto* renderCtx = g_lume.engine->GetInterface<RENDER_NS::IRenderContext>();
    LOGI("Engine->GetInterface<IRenderContext> after Init = 0x%{public}lx", (unsigned long)(uintptr_t)renderCtx);

    LOGI("Engine initialized (version: %{public}s)",
        BASE_NS::string(g_lume.engine->GetVersion()).c_str());

    return true;
}

bool InitRenderContext()
{
    LOGI("Creating render context...");

    auto* engineClassFactory = g_lume.engine->GetInterface<CORE_NS::IClassFactory>();
    if (!engineClassFactory) {
        LOGE("Engine does not expose IClassFactory");
        return false;
    }

    auto renderCtxPtr = CORE_NS::CreateInstance<RENDER_NS::IRenderContext>(*engineClassFactory, RENDER_NS::UID_RENDER_CONTEXT);
    if (!renderCtxPtr) {
        LOGE("Failed to create IRenderContext");
        return false;
    }
    g_lume.renderContext = BASE_NS::move(renderCtxPtr);
    LOGI("IRenderContext instance created");

    RENDER_NS::RenderCreateInfo renderCI {};
    renderCI.applicationInfo.name = "LumeApp";
    renderCI.deviceCreateInfo.backendType = RENDER_NS::DeviceBackendType::VULKAN;

    // CRITICAL: RenderContext::Init() registers rendershaders → rofsRndr://shaders/, etc.
    // Since RENDER_EMBEDDED_ASSETS_ENABLED=0, rofsRndr filesystem doesn't exist.
    // Register rofsRndr as a path alias to our file:// render shader directory BEFORE Init().
    {
        auto& fm = g_lume.engine->GetFileManager();
        const std::string shaderBase = g_filesDir + "/shaders";
        const BASE_NS::string renderUri = BASE_NS::string("file://") + shaderBase.c_str() + "/render/";
        fm.RegisterPath("rofsRndr", renderUri, false);
        LOGI("Registered rofsRndr → %{public}s (before RenderContext::Init)", renderUri.c_str());
    }

    auto result = g_lume.renderContext->Init(renderCI);
    if (result != RENDER_NS::RenderResultCode::RENDER_SUCCESS) {
        LOGE("IRenderContext::Init failed (result=%{public}u)", static_cast<uint32_t>(result));
        return false;
    }
    LOGI("Render context initialized with Vulkan backend");

    g_lume.device = &g_lume.renderContext->GetDevice();
    g_lume.renderer = &g_lume.renderContext->GetRenderer();

    // Create swapchain handle — only use CreateSwapchainHandle, not CreateSwapchain
    RENDER_NS::SwapchainCreateInfo swapCI {};
    swapCI.window.window = reinterpret_cast<uintptr_t>(g_lume.nativeWindow);
    swapCI.swapchainFlags =
        RENDER_NS::CORE_SWAPCHAIN_COLOR_BUFFER_BIT |
        RENDER_NS::CORE_SWAPCHAIN_DEPTH_BUFFER_BIT |
        RENDER_NS::CORE_SWAPCHAIN_SRGB_BIT;
    swapCI.imageUsageFlags = RENDER_NS::CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    g_lume.swapchainHandle = g_lume.device->CreateSwapchainHandle(swapCI);
    LOGI("Swapchain handle created: valid=%{public}d", g_lume.swapchainHandle ? 1 : 0);

    return true;
}

bool InitGraphicsContext()
{
    LOGI("Creating 3D graphics context...");

    auto* classFactory = g_lume.renderContext->GetInterface<CORE_NS::IClassFactory>();
    if (!classFactory) {
        LOGE("RenderContext does not expose IClassFactory");
        return false;
    }

    g_lume.graphicsContext = CORE_NS::CreateInstance<CORE3D_NS::IGraphicsContext>(
        *classFactory, CORE3D_NS::UID_GRAPHICS_CONTEXT);
    if (!g_lume.graphicsContext) {
        LOGE("Failed to create IGraphicsContext");
        return false;
    }

    CORE3D_NS::IGraphicsContext::CreateInfo gfxCI {};

    // CRITICAL: GraphicsContext::Init() registers 3dshaders → rofs3D://shaders/, etc.
    // In normal OpenHarmony builds, rofs3D is an embedded ROFS filesystem in the .so.
    // Since CORE3D_EMBEDDED_ASSETS_ENABLED=0, we need to register rofs3D as a path
    // pointing to our file:// shader directory BEFORE Init() runs.
    {
        auto& fm = g_lume.engine->GetFileManager();
        const std::string shaderBase = g_filesDir + "/shaders";
        const BASE_NS::string tdUri = BASE_NS::string("file://") + shaderBase.c_str() + "/3d/";
        fm.RegisterPath("rofs3D", tdUri, false);
        LOGI("Registered rofs3D → %{public}s (before GraphicsContext::Init)", tdUri.c_str());
    }

    g_lume.graphicsContext->Init(gfxCI);
    LOGI("Graphics context initialized");

    // Load splat shaders
    {
        RENDER_NS::IShaderManager::ShaderFilePathDesc desc;
        desc.shaderPath = "splatshaders://";
        desc.pipelineLayoutPath = "splatshaders://";
        desc.shaderStatePath = "splatshaders://";
        desc.vertexInputDeclarationPath = "splatshaders://";
        g_lume.renderContext->GetDevice().GetShaderManager().LoadShaderFiles(desc);
        LOGI("Splat shaders loaded");
    }

    return true;
}

bool InitEcs()
{
    LOGI("Creating ECS...");

    g_lume.ecs = g_lume.engine->CreateEcs();
    if (!g_lume.ecs) {
        LOGE("Failed to create ECS");
        return false;
    }

    // Load the system graph (defines which systems run and in what order).
    // Try to load from the 3D plugin's embedded assets first.
    auto* sgFactory = CORE_NS::GetInstance<CORE_NS::ISystemGraphLoaderFactory>(
        CORE_NS::UID_SYSTEM_GRAPH_LOADER);
    if (sgFactory) {
        auto loader = sgFactory->Create(g_lume.engine->GetFileManager());
        // Try the 3D plugin's embedded asset path.
        auto loadResult = loader->Load("3d://systemGraph.json", *g_lume.ecs);
        if (!loadResult.success) {
            LOGW("Failed to load 3d://systemGraph.json: %{public}s, trying app://",
                loadResult.error.c_str());
            // Fallback: try app:// which maps to our rawfile dir.
            loadResult = loader->Load("app://systemGraph.json", *g_lume.ecs);
            if (!loadResult.success) {
                LOGW("Failed to load app://systemGraph.json: %{public}s, "
                     "ECS will use default systems", loadResult.error.c_str());
            } else {
                LOGI("Loaded system graph from app://systemGraph.json");
            }
        } else {
            LOGI("Loaded system graph from 3d://systemGraph.json");
        }
    } else {
        LOGW("ISystemGraphLoaderFactory not available, using default ECS init");
    }

    g_lume.ecs->Initialize();
    LOGI("ECS initialized");
    return true;
}

static CORE_NS::Entity g_cameraEntity {};

// Large scenes get a light sky clear colour: an empty view then looks like sky
// instead of a black screen (which is indistinguishable from a broken frame).
static void SetClearColor(float r, float g, float b)
{
    if (!g_lume.ecs) {
        return;
    }
    auto* camMgr = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(*g_lume.ecs);
    if (!camMgr) {
        return;
    }
    auto handle = camMgr->Write(g_cameraEntity);
    if (handle) {
        handle->clearColorValue = { r, g, b, 1.0f };
        LOGI("Clear colour set to (%.2f,%.2f,%.2f)", r, g, b);
    }
}

void SetupCameraAndLight()
{
    if (!g_lume.graphicsContext || !g_lume.ecs) {
        LOGW("Cannot set up camera/light: missing graphicsContext or ECS");
        return;
    }

    auto& sceneUtil = g_lume.graphicsContext->GetSceneUtil();
    auto& ecs = *g_lume.ecs;

    // Create a camera looking at the origin from a reasonable distance.
    const BASE_NS::Math::Vec3 cameraPos { 0.0f, 1.0f, 3.0f };
    const BASE_NS::Math::Quat cameraRot {}; // identity — we use LookAt below
    const float zNear = 0.1f;
    const float zFar = 100.0f;
    const float fovDeg = 60.0f;

    CORE_NS::Entity cameraEntity = sceneUtil.CreateCamera(
        ecs, cameraPos, cameraRot, zNear, zFar, fovDeg);
    g_cameraEntity = cameraEntity;

    // Point the camera at the origin.
    const BASE_NS::Math::Vec3 target { 0.0f, 0.0f, 0.0f };
    const BASE_NS::Math::Vec3 up { 0.0f, 1.0f, 0.0f };
    sceneUtil.CameraLookAt(ecs, cameraEntity, cameraPos, target, up);

    // Update the camera viewport to match surface resolution.
    const BASE_NS::Math::UVec2 resolution {
        static_cast<uint32_t>(g_lume.width),
        static_cast<uint32_t>(g_lume.height)
    };
    sceneUtil.UpdateCameraViewport(ecs, cameraEntity, resolution);

    // Set a bright blue clear color so we can tell if the camera is rendering to screen
    auto* camMgr = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecs);
    if (camMgr) {
        auto camHandle = camMgr->Write(cameraEntity);
        if (camHandle) {
            camHandle->clearColorValue = { 0.006f, 0.004f, 0.002f, 1.0f }; // warm black (matches web viewer bg)
            // CRITICAL: Without MAIN_CAMERA_BIT, RenderNodeDefaultCameraController renders
            // to an internal buffer instead of CORE_DEFAULT_BACKBUFFER (swapchain).
            // The scene renders correctly but nothing gets presented to screen.
            camHandle->sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            // CRITICAL: CLEAR_COLOR_BIT forces loadOp=clear on color attachment.
            // Without it, the default LWRP pipeline uses loadOp=dont_care and never clears.
            camHandle->pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT;
            LOGI("Camera: clear=dark-neutral, MAIN_CAMERA_BIT + CLEAR_COLOR_BIT set");
        }
    }

    // CRITICAL: Add RenderConfigurationComponent to enable render pipeline creation.
    // Without this, RenderSystem won't create any render node graphs and the screen stays black.
    auto* rcMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecs);
    if (rcMgr) {
        rcMgr->Create(cameraEntity);

        // Create an environment entity with a visible background color
        auto* envMgr = CORE_NS::GetManager<CORE3D_NS::IEnvironmentComponentManager>(ecs);
        if (envMgr) {
            CORE_NS::Entity envEntity = ecs.GetEntityManager().Create();
            envMgr->Create(envEntity);
            // Link environment to the render configuration
            auto rcHandle = rcMgr->Write(cameraEntity);
            if (rcHandle) {
                rcHandle->environment = envEntity;
            }
            LOGI("EnvironmentComponent created and linked to camera");
        }

        LOGI("RenderConfigurationComponent added to camera entity (CREATE_RNGS_BIT enabled by default)");
    } else {
        LOGE("IRenderConfigurationComponentManager not found — rendering will be black");
    }

    LOGI("Camera created and positioned at (0, 1, 3) looking at origin");

    // Create a directional light.
    CORE3D_NS::LightComponent lightComp {};
    lightComp.type = CORE3D_NS::LightComponent::Type::DIRECTIONAL;
    lightComp.intensity = 1.0f;
    lightComp.color = { 1.0f, 1.0f, 1.0f };

    const BASE_NS::Math::Vec3 lightPos { 2.0f, 4.0f, 2.0f };
    const BASE_NS::Math::Quat lightRot {}; // identity
    sceneUtil.CreateLight(ecs, lightComp, lightPos, lightRot);

    LOGI("Directional light created at (2, 4, 2)");
}

// Demo models and their per-model rendering configuration.
struct DemoModelConfig {
    const char* file;
    float fovYDeg;
    float shDeg1;
    float shDeg23;
    // Optional recorded viewpoint (orientation only). When hasView is false an
    // identity basis is used. The orbit target is the subject centroid unless
    // target[] is non-zero.
    bool hasView;
    float right[3];
    float up[3];
    float back[3];
    float target[3];
    // <= 0 means "derive from the robust subject radius".
    float insideDistance;
    float farDistance;
    // Non-null for LOD scenes: directory under <files>/assets/lod holding
    // default.scene.json + lod-meta.json. `file` is empty for these entries.
    const char* scene;
};
static const DemoModelConfig kDemoModels[] = {
    // Standard 3DGS PLY. The bounds cover a whole room, so the auto-frame lands
    // inside the geometry; use the viewpoint recorded to match the web capture
    // (keeping it also makes the far view reproduce the web reference framing).
    { "fruit_bowl.ply", 60.0f, 0.0f, 0.0f, true,
        { -0.717920f, 0.024674f, -0.695681f },
        { 0.326028f, -0.871066f, -0.367345f },
        { -0.615049f, -0.490535f, 0.617311f },
        { 3.886603f, -1.901034f, 0.267847f },
        0.f, 57.762306f },
    // Same scene as the PLY above, compressed as SOG v2 (13 MB instead of 177 MB).
    // The recorded viewpoint is shared so the two can be compared directly.
    { "fruit_bowl.sog", 60.0f, 0.0f, 0.0f, true,
        { -0.717920f, 0.024674f, -0.695681f },
        { 0.326028f, -0.871066f, -0.367345f },
        { -0.615049f, -0.490535f, 0.617311f },
        { 3.886603f, -1.901034f, 0.267847f },
        0.f, 57.762306f },
    // Compressed .splat, no SH data.
    { "godzilla.splat", 80.0f, 0.0f, 0.0f, false, {}, {}, {}, {}, 0.0f, 0.0f },
    // UWA glTF with vector-quantised SH. The bounds are dominated by the
    // environment shell, so use the framing that matches the reference capture.
    { "doll.glb", 60.0f, 1.0f, 1.0f, false, {}, {}, {}, {}, 42.0f, 250.0f, nullptr },
    // Large scene with per-tile LOD (SOG tiles). Camera and framing come from
    // default.scene.json, so the entry only names the directory.
    { "", 60.0f, 0.0f, 0.0f, false, {}, {}, {}, {}, 0.0f, 0.0f, "lod/syp_uav_0611" },
};
static constexpr int kDemoModelCount = (int)(sizeof(kDemoModels) / sizeof(kDemoModels[0]));
static int g_modelIndex = 3;  // doll.glb by default (must track kDemoModels order)
static int g_pendingModelIndex = -1;  // set from ArkTS, applied on the render thread
static int g_pendingCameraMode = -1;  // -1 = use the model default, 0 = orbit, 1 = first person
// LOD splat budget, switchable from ArkTS (dropdown) and remembered across model
// switches: 1.5M is the densest, 1M is the default (matches the scene's mobile cap).
static std::atomic<uint32_t> g_lodBudgetPreference { 1000000 };

// Robust framing statistics. Raw bounds are misleading: a few outlier
// gaussians (room walls, an environment sphere) can stretch them far beyond
// the actual subject, which made the camera start inside the geometry.
struct SplatFraming {
    float cx, cy, cz;   // opacity-weighted centroid
    float r50, r80, r90;  // percentile radii around the centroid
};

static SplatFraming ComputeSplatFraming(const SplatData& data)
{
    SplatFraming f {};
    const uint32_t n = data.gaussianCount;
    if (n == 0 || data.gaussians.empty()) {
        return f;
    }

    constexpr uint32_t kBins = 1024;
    double sx = 0.0, sy = 0.0, sz = 0.0, sw = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const auto& g = data.gaussians[i];
        const double w = 1.0 / (1.0 + std::exp(-(double)g.opacity));
        sx += (double)g.px * w;
        sy += (double)g.py * w;
        sz += (double)g.pz * w;
        sw += w;
    }
    if (sw <= 0.0) {
        return f;
    }
    const float cx = (float)(sx / sw);
    const float cy = (float)(sy / sw);
    const float cz = (float)(sz / sw);

    float rmax = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const auto& g = data.gaussians[i];
        const float dx = g.px - cx, dy = g.py - cy, dz = g.pz - cz;
        const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (r > rmax) rmax = r;
    }
    if (rmax <= 0.0f) {
        return f;
    }

    std::vector<uint32_t> hist(kBins, 0u);
    const float binScale = (float)kBins / rmax;
    for (uint32_t i = 0; i < n; i++) {
        const auto& g = data.gaussians[i];
        const float dx = g.px - cx, dy = g.py - cy, dz = g.pz - cz;
        const float r = std::sqrt(dx * dx + dy * dy + dz * dz);
        uint32_t b = (uint32_t)(r * binScale);
        if (b >= kBins) b = kBins - 1;
        hist[b]++;
    }

    auto radiusAt = [&](float fraction) {
        const uint32_t target = (uint32_t)(fraction * (float)n);
        uint32_t acc = 0;
        for (uint32_t b = 0; b < kBins; b++) {
            acc += hist[b];
            if (acc >= target) {
                return (float)(b + 1) / binScale;
            }
        }
        return rmax;
    };

    f.cx = cx; f.cy = cy; f.cz = cz;
    f.r50 = radiusAt(0.50f);
    f.r80 = radiusAt(0.80f);
    f.r90 = radiusAt(0.90f);
    return f;
}

// Camera mode: orbit (small scenes, focus at the subject) vs first person
// (large scenes: rotate in place, walk through). Switching keeps the eye fixed.
static void SetFirstPerson(bool enabled)
{
    if (enabled == g_orbitCam.firstPerson) {
        return;
    }
    float ex, ey, ez;
    g_orbitCam.GetPosition(ex, ey, ez);
    if (enabled) {
        // Move the pivot to the eye so the camera rotates in place.
        g_orbitCam.targetX = ex;
        g_orbitCam.targetY = ey;
        g_orbitCam.targetZ = ez;
        g_orbitCam.distance = 0.0f;
    } else if (g_orbitCam.hasPivot) {
        // Orbit around the scene pivot while keeping the current eye position.
        g_orbitCam.targetX = g_orbitCam.pivotX;
        g_orbitCam.targetY = g_orbitCam.pivotY;
        g_orbitCam.targetZ = g_orbitCam.pivotZ;
        const float dx = ex - g_orbitCam.pivotX;
        const float dy = ey - g_orbitCam.pivotY;
        const float dz = ez - g_orbitCam.pivotZ;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        d = std::max(d, g_orbitCam.minDistance);
        g_orbitCam.distance = d;
    } else {
        // Back off along the view direction so the subject is in front again.
        const float back = g_orbitCam.fpBackoff > 0.0f ? g_orbitCam.fpBackoff
                                                      : g_orbitCam.insideDistance;
        g_orbitCam.targetX = ex - g_orbitCam.backX * back;
        g_orbitCam.targetY = ey - g_orbitCam.backY * back;
        g_orbitCam.targetZ = ez - g_orbitCam.backZ * back;
        g_orbitCam.distance = back;
    }
    g_orbitCam.firstPerson = enabled;
    g_orbitCam.LogState(enabled ? "first-person" : "orbit");
    g_forceRenderFrames = 3;
}

// ---- LOD scene streaming ----
// Render path selector: 0 = legacy custom splat pipeline, 1 = LumeGS.
static int g_renderPath = 1;
static CORE_NS::Entity g_gsConfigEntity {};
static RENDER_NS::RenderHandleReference g_gsRngHandle;
static BASE_NS::string g_gsModelFile { "doll.glb" };
static bool g_gsModelDirty = false;
static CORE_NS::Entity g_gsModelEntity {};
static uint32_t g_gsLogCounter = 0;
static bool g_gsWasRenderable = false;

// Dedicated LumeGS tiled LOD host state, mirroring the system-side GSPlugin:
// RebuildTileSelection -> ProcessPendingTileCreates -> ProcessVisibleTiles ->
// ProcessPreloadTiles, then RequestRender on camera/tile/residency/loading flags.
static lod::Scene g_gsLodScene;
static std::vector<CORE_NS::Entity> g_gsLodTileEntities;
static std::vector<int> g_gsLodFineLevel;      // desired visible fine level, -1 = hidden
static std::vector<uint8_t> g_gsLodPreload;    // surrounding ring leaf
static std::vector<int> g_gsLodShownLevel;     // level currently visible, -1 = none
static std::vector<uint8_t> g_gsLodTilePending; // per leaf*8, create queued
static std::vector<uint32_t> g_gsLodVisibleOrder; // nearest-first visible leaves
static std::vector<uint32_t> g_gsLodRingOrder;    // nearest-first preload ring
struct GsLodPendingCreate {
    uint32_t leaf = 0;
    int level = 0;
    bool preload = false;
};
static std::vector<GsLodPendingCreate> g_gsLodPending;
static size_t g_gsLodPendingHead = 0;
static bool g_gsLodActive = false;
static bool g_gsLodSelectionDirty = true;
static float g_gsLodLastView[16] {};
static float g_gsLodLastProj[16] {};
static uint32_t g_gsLodLastBudget = 0;
static size_t g_gsLodLastResident = static_cast<size_t>(-1);

static bool EnsureGsConfiguration()
{
    if (!g_lume.ecs) {
        return false;
    }
    auto& ecs = *g_lume.ecs;
    auto* cfgMgr = CORE_NS::GetManager<GS::IGsConfigurationComponentManager>(ecs);
    if (!cfgMgr) {
        return false;
    }
    if (g_gsConfigEntity == CORE_NS::Entity {}) {
        g_gsConfigEntity = ecs.GetEntityManager().Create();
        cfgMgr->Create(g_gsConfigEntity);
        if (auto cfg = cfgMgr->Write(g_gsConfigEntity)) {
            // Match the system-side GSPlugin configuration: GPU sort (required
            // by the tiled pool), 24-bit keys, sRGB conversion and its quality
            // tuning.
            cfg->useGpuSort = true;
            cfg->use24BitGpuSort = true;
            cfg->convertGsColorSrgb = true;
            cfg->renderCovarianceStddev = 2.25f;
            cfg->renderSharpeningCoefficient = 0.75f;
        }
        LOGI("GS path: configuration entity created");
    }
    return true;
}

static void InitGsRenderPath(const char* modelFile)
{
    if (!g_lume.ecs) {
        return;
    }
    auto& ecs = *g_lume.ecs;
    {
        auto* gsSys = CORE_NS::GetSystem<GS::IGsSystem>(ecs);
        auto* tileSys = CORE_NS::GetSystem<GS::IGsTilingSystem>(ecs);
        const auto systems = ecs.GetSystems();
        LOGI("GS path: systems=%{public}zu gsSystem=%{public}d tilingSystem=%{public}d",
            systems.size(), gsSys != nullptr, tileSys != nullptr);
    }
    auto* gsMgr = CORE_NS::GetManager<GS::IGsComponentManager>(ecs);
    auto* tfMgr = CORE_NS::GetManager<CORE3D_NS::ITransformComponentManager>(ecs);
    if (!EnsureGsConfiguration() || !gsMgr || !tfMgr) {
        LOGE("GS path: component managers missing (cfg=%d gs=%d tf=%d)",
            g_gsConfigEntity != CORE_NS::Entity {}, gsMgr != nullptr, tfMgr != nullptr);
        return;
    }
    if (g_gsModelEntity == CORE_NS::Entity {}) {
        // The composite material is rendered as a regular render mesh, so the
        // entity needs the full scene-node component set (world matrix in
        // particular). A bare entity with only Transform + GsComponent is not
        // picked up by RenderSystem.
        auto* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecs);
        if (nodeSystem) {
            if (auto* node = nodeSystem->CreateNode()) {
                g_gsModelEntity = node->GetEntity();
            }
        }
        if (g_gsModelEntity == CORE_NS::Entity {}) {
            g_gsModelEntity = ecs.GetEntityManager().Create();
            tfMgr->Create(g_gsModelEntity);
        }
        if (auto tf = tfMgr->Write(g_gsModelEntity)) {
            tf->position = { 0.0f, 0.0f, 0.0f };
            tf->scale = { 1.0f, 1.0f, 1.0f };
        }
        gsMgr->Create(g_gsModelEntity);
    }
    if (auto gs = gsMgr->Write(g_gsModelEntity)) {
        const BASE_NS::string uri = BASE_NS::string("file://") + g_filesDir.c_str() + "/assets/" + modelFile;
        gs->gsPath = uri;
        gs->computeLods = false;
        LOGI("GS path: loading '%{public}s'", uri.c_str());
    }
    // The GS node is inserted into the camera scene RNG (see
    // core3d_rng_cam_scene_lwrp.rng); a separate RNG runs after the scene pass
    // and would only feed the quad stale targets.
    if (false && !g_gsRngHandle && g_lume.renderContext) {
        auto& rngMgr = g_lume.renderContext->GetRenderNodeGraphManager();
        RENDER_NS::RenderNodeGraphDesc rngDesc {};
        rngDesc.renderNodeGraphName = "GaussianSplatGS_RNG";
        RENDER_NS::RenderNodeDesc nodeDesc {};
        nodeDesc.typeName = "RenderNodeGsRender";
        nodeDesc.nodeName = "GsRenderNode";
        rngDesc.nodes.push_back(BASE_NS::move(nodeDesc));
        g_gsRngHandle = rngMgr.Create(
            RENDER_NS::IRenderNodeGraphManager::RenderNodeGraphUsageType::RENDER_NODE_GRAPH_STATIC, rngDesc);
        LOGI("GS path: render node graph %{public}s", g_gsRngHandle ? "valid" : "INVALID");
    }
}

static void PollGsRenderPath()
{
    if (g_renderPath != 1 || !g_lume.ecs || g_gsLodActive) {
        return;
    }
    if (g_gsModelEntity == CORE_NS::Entity {}) {
        InitGsRenderPath(g_gsModelFile.c_str());  // first frame after the ECS exists
        return;
    }
    auto* gsMgr = CORE_NS::GetManager<GS::IGsComponentManager>(*g_lume.ecs);
    if (!gsMgr) {
        return;
    }
    if (g_gsModelDirty) {
        g_gsModelDirty = false;
        g_gsWasRenderable = false;
        gsMgr->Destroy(g_gsModelEntity);
        gsMgr->Create(g_gsModelEntity);
        if (auto gs = gsMgr->Write(g_gsModelEntity)) {
            const BASE_NS::string uri =
                BASE_NS::string("file://") + g_filesDir.c_str() + "/assets/" + g_gsModelFile.c_str();
            gs->gsPath = uri;
            gs->computeLods = false;
            LOGI("GS path: switching to '%{public}s'", uri.c_str());
        }
    }
    auto gs = gsMgr->Read(g_gsModelEntity);
    if (!gs) {
        return;
    }
    // Loading is async and finishes after the startup render-on-demand window.
    // Schedule frames when the primitive becomes renderable so the first
    // composed GS image is actually drawn.
    const bool renderable = gs->isLoaded && gs->isPrimitiveValid;
    if (renderable && !g_gsWasRenderable) {
        g_gsWasRenderable = true;
        // Keep the warmup short so touch input is not starved by forced frames
        // after a model switch (30 frames delayed the first drag by ~0.7s).
        g_forceRenderFrames = 8;
        LOGI("GS path: model renderable, scheduling frames");
    } else if (!renderable) {
        g_gsWasRenderable = false;
    }
    if (((++g_gsLogCounter) % 60u) != 0u) {
        return;
    }
    {
        auto* gsSys = CORE_NS::GetSystem<GS::IGsSystem>(*g_lume.ecs);
        auto* tileSys = CORE_NS::GetSystem<GS::IGsTilingSystem>(*g_lume.ecs);
        LOGI("GS path: systems=%{public}zu gsSystem=%{public}d tilingSystem=%{public}d",
            g_lume.ecs->GetSystems().size(), gsSys != nullptr, tileSys != nullptr);
    }
    LOGI("GS path: loaded=%{public}d primitiveValid=%{public}d", gs->isLoaded ? 1 : 0,
        gs->isPrimitiveValid ? 1 : 0);
}

static lod::Streamer g_lodStreamer;
static bool g_lodActive = false;
static float g_lastLodView[16] = {};

// Drop the LumeGS model entity (and the composite mesh attached to it) when
// switching to the legacy LOD path. The GS node stays in the scene RNG but has
// no primitives to draw.
static void DestroyGsRenderPath()
{
    if (!g_lume.ecs || g_gsModelEntity == CORE_NS::Entity {}) {
        return;
    }
    auto& ecs = *g_lume.ecs;
    auto* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecs);
    if (nodeSystem) {
        if (auto* node = nodeSystem->GetNode(g_gsModelEntity)) {
            nodeSystem->DestroyNode(*node);
        }
    } else {
        ecs.GetEntityManager().Destroy(g_gsModelEntity);
    }
    g_gsModelEntity = {};
    g_gsWasRenderable = false;
    g_gsModelDirty = false;
    LOGI("GS path: model entity destroyed");
}

static bool FileExists(const std::string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static const char* SceneBaseName(const char* scene)
{
    const char* slash = strrchr(scene, '/');
    return slash ? slash + 1 : scene;
}

void LoadLodScene(const DemoModelConfig& cfg)
{
    // Prefer the scene pushed into the sandbox; fall back to the HAP-bundled
    // LOD2+LOD3 subset (rawfile/assets/lod_fallback/<name>).
    std::string dir = g_filesDir + "/assets/" + cfg.scene;
    if (!FileExists(dir + "/lod-meta.json")) {
        const std::string fallback =
            g_filesDir + "/assets/lod_fallback/" + std::string(SceneBaseName(cfg.scene));
        if (FileExists(fallback + "/lod-meta.json")) {
            dir = fallback;
        }
    }
    LOGI("LOD scene dir: %{public}s", dir.c_str());

    // The scene asks for 1M splats, which leaves large parts of an aerial view at
    // the coarsest band. 1.5M costs a bit of fill rate but noticeably raises the
    // average density.
    const uint32_t lodBudget = g_lodBudgetPreference.load();
    if (!g_lodStreamer.Start(dir, lodBudget, 0)) {
        LOGE("LOD scene failed to start");
        return;
    }
    g_lodActive = true;
    SetClearColor(0.62f, 0.72f, 0.82f);
    g_gaussianBackground[0] = 0.62f;
    g_gaussianBackground[1] = 0.72f;
    g_gaussianBackground[2] = 0.82f;
    g_gaussianBackground[3] = 1.0f;

    const lod::Scene& scene = g_lodStreamer.GetScene();
    g_orbitCam.fovYDeg = cfg.fovYDeg;
    g_orbitCam.shDeg1 = cfg.shDeg1;
    g_orbitCam.shDeg23 = cfg.shDeg23;

    // Camera from the scene metadata: eye = cameraInfo.position, target = lookAt.
    float bx = scene.camPos[0] - scene.camLookAt[0];
    float by = scene.camPos[1] - scene.camLookAt[1];
    float bz = scene.camPos[2] - scene.camLookAt[2];
    float len = std::sqrt(bx * bx + by * by + bz * bz);
    if (len < 1e-3f) {
        bx = 0.0f;
        by = 0.0f;
        bz = 1.0f;
        len = 1.0f;
    }
    g_orbitCam.backX = bx / len;
    g_orbitCam.backY = by / len;
    g_orbitCam.backZ = bz / len;
    // right = normalize(up x back), up = back x right
    float rx = 1.0f * g_orbitCam.backZ - 0.0f * g_orbitCam.backY;
    float ry = 0.0f * g_orbitCam.backX - 0.0f * g_orbitCam.backZ;
    float rz = 0.0f * g_orbitCam.backY - 1.0f * g_orbitCam.backX;
    float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-4f) {
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
        rl = 1.0f;
    }
    g_orbitCam.rightX = rx / rl;
    g_orbitCam.rightY = ry / rl;
    g_orbitCam.rightZ = rz / rl;
    g_orbitCam.upX = g_orbitCam.backY * g_orbitCam.rightZ - g_orbitCam.backZ * g_orbitCam.rightY;
    g_orbitCam.upY = g_orbitCam.backZ * g_orbitCam.rightX - g_orbitCam.backX * g_orbitCam.rightZ;
    g_orbitCam.upZ = g_orbitCam.backX * g_orbitCam.rightY - g_orbitCam.backY * g_orbitCam.rightX;

    // First-person pose: the pivot is the eye itself (the view direction is
    // still towards lookAt). Switching to orbit later backs off by `len`.
    g_orbitCam.targetX = scene.camPos[0];
    g_orbitCam.targetY = scene.camPos[1];
    g_orbitCam.targetZ = scene.camPos[2];
    g_orbitCam.distance = 0.0f;

    float ex = scene.boundsMax[0] - scene.boundsMin[0];
    float ey = scene.boundsMax[1] - scene.boundsMin[1];
    float ez = scene.boundsMax[2] - scene.boundsMin[2];
    const float diag = std::sqrt(ex * ex + ey * ey + ez * ez);
    // No depth buffer -> the near plane only has to stop clipping splats that are
    // right in front of the camera. Keeping it in world units (0.1) instead of a
    // fraction of the scene diagonal avoids the "zoom in -> everything culled"
    // black screen on a solid scene.
    g_orbitCam.nearPlane = 0.1f;
    g_orbitCam.farPlane = (len + diag) * 2.0f;
    g_orbitCam.minDistance = std::max(1.0f, diag * 0.002f);
    g_orbitCam.insideDistance = std::max(1.0f, diag * 0.05f);
    g_orbitCam.farDistance = len;
    // Start in the far framing so the first double-tap zooms into the scene.
    g_orbitCam.insideView = false;
    // Large scenes are navigated in first person; the mode button can switch.
    g_orbitCam.firstPerson = true;
    g_orbitCam.fpMoveScale = std::max(0.05f, diag * 0.0008f);
    // Returning to orbit mode backs off to the reference framing distance and
    // orbits around the scene's look-at point.
    g_orbitCam.fpBackoff = len;
    g_orbitCam.pivotX = scene.camLookAt[0];
    g_orbitCam.pivotY = scene.camLookAt[1];
    g_orbitCam.pivotZ = scene.camLookAt[2];
    g_orbitCam.hasPivot = true;
    // Roaming is confined to the scene bounds plus a margin, so walking cannot
    // leave the model behind (which looks like a black screen).
    for (int i = 0; i < 3; i++) {
        const float margin = (scene.boundsMax[i] - scene.boundsMin[i]) * 0.15f;
        g_orbitCam.boundsMin[i] = scene.boundsMin[i] - margin;
        g_orbitCam.boundsMax[i] = scene.boundsMax[i] + margin;
    }
    g_orbitCam.hasBounds = true;
    g_orbitCam.LogState("lod-initial");

    // No CPU splat array: the node waits for the first streamed assembly.
    g_gaussianPoolMode = 1;  // patches only; never rewrite the whole pool
    g_gaussianSplatData = nullptr;
    g_gaussianSplatCount = 0;
    g_gaussianSplatCapacity = g_lodStreamer.Capacity();
    g_gaussianShData = nullptr;
    g_gaussianDataReady = false;
    ++g_gaussianSplatGeneration;

    // RNG for the gaussian splatting node (same as the single-file path)
    auto& rngMgr = g_lume.renderContext->GetRenderNodeGraphManager();
    RENDER_NS::RenderNodeGraphDesc rngDesc {};
    rngDesc.renderNodeGraphName = "GaussianSplatRNG";

    RENDER_NS::RenderNodeDesc nodeDesc {};
    nodeDesc.typeName = "RenderNodeGaussianSplatting";
    nodeDesc.nodeName = "GaussianSplatNode";
    rngDesc.nodes.push_back(BASE_NS::move(nodeDesc));

    g_lume.splatRngHandle = rngMgr.Create(
        RENDER_NS::IRenderNodeGraphManager::RenderNodeGraphUsageType::RENDER_NODE_GRAPH_STATIC,
        rngDesc);
    LOGI("LOD RNG created: valid=%{public}d, budget=%{public}u", g_lume.splatRngHandle ? 1 : 0,
        g_lodStreamer.Budget());

    if (g_pendingCameraMode >= 0) {
        SetFirstPerson(g_pendingCameraMode != 0);
        g_pendingCameraMode = -1;
    }
}

static void ReleaseSplatModel()
{
    // Any decode still running belongs to the model we are leaving.
    ++g_splatDecodeGeneration;
    g_splatDecodeQueued = false;

    // Release the RNG first: it owns the RenderNode (and its GPU buffers),
    // which must not outlive the CPU-side splat arrays.
    if (g_lodActive) {
        g_lodStreamer.Stop();
        g_lodActive = false;
        memset(g_lastLodView, 0, sizeof(g_lastLodView));
    }
    g_lume.splatRngHandle = {};
    g_gaussianSplatData = nullptr;
    g_gaussianSplatCount = 0;
    g_gaussianSplatCapacity = 0;
    g_gaussianPoolSize = 0;
    g_gaussianLiveRanges = nullptr;
    g_gaussianLiveRangeCount = 0;
    g_gaussianDirtyRanges = nullptr;
    g_gaussianDirtyRangeCount = 0;
    ++g_gaussianSplatGeneration;
    g_gaussianShData = nullptr;
    g_gaussianDataReady = false;

    // Keep the decoded CPU data for the next switch instead of discarding it.
    // The legacy GPU node is gone, but re-decoding 400k+ gaussians on the
    // render thread made every City -> Doll switch stall for ~0.5s.
    if (!g_splatDataInUsePath.empty() && !g_lume.splatData.gaussians.empty()) {
        g_cachedSplatPath = g_splatDataInUsePath;
        g_cachedSplatData = std::move(g_lume.splatData);
    }
    g_splatDataInUsePath.clear();

    g_lume.splatData = SplatData {};
    g_lume.splatLoaded = false;
}

static std::string ResolveLodDir(const DemoModelConfig& cfg)
{
    std::string dir = g_filesDir + "/assets/" + cfg.scene;
    if (!FileExists(dir + "/lod-meta.json")) {
        const std::string fallback =
            g_filesDir + "/assets/lod_fallback/" + std::string(SceneBaseName(cfg.scene));
        if (FileExists(fallback + "/lod-meta.json")) {
            dir = fallback;
        }
    }
    return dir;
}

// Reuse the legacy LOD camera framing (metadata position/lookAt, first-person
// eye, scene-sized clip planes) for the LumeGS tiling path.
static void ApplyGsLodCamera(const lod::Scene& scene, const DemoModelConfig& cfg)
{
    SetClearColor(0.62f, 0.72f, 0.82f);
    g_gaussianBackground[0] = 0.62f;
    g_gaussianBackground[1] = 0.72f;
    g_gaussianBackground[2] = 0.82f;
    g_gaussianBackground[3] = 1.0f;

    g_orbitCam.fovYDeg = cfg.fovYDeg;
    g_orbitCam.shDeg1 = cfg.shDeg1;
    g_orbitCam.shDeg23 = cfg.shDeg23;

    float bx = scene.camPos[0] - scene.camLookAt[0];
    float by = scene.camPos[1] - scene.camLookAt[1];
    float bz = scene.camPos[2] - scene.camLookAt[2];
    float len = std::sqrt(bx * bx + by * by + bz * bz);
    if (len < 1e-3f) {
        bx = 0.0f;
        by = 0.0f;
        bz = 1.0f;
        len = 1.0f;
    }
    g_orbitCam.backX = bx / len;
    g_orbitCam.backY = by / len;
    g_orbitCam.backZ = bz / len;
    // right = normalize(up x back), up = back x right
    float rx = 1.0f * g_orbitCam.backZ - 0.0f * g_orbitCam.backY;
    float ry = 0.0f * g_orbitCam.backX - 0.0f * g_orbitCam.backZ;
    float rz = 0.0f * g_orbitCam.backY - 1.0f * g_orbitCam.backX;
    float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-4f) {
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
        rl = 1.0f;
    }
    g_orbitCam.rightX = rx / rl;
    g_orbitCam.rightY = ry / rl;
    g_orbitCam.rightZ = rz / rl;
    g_orbitCam.upX = g_orbitCam.backY * g_orbitCam.rightZ - g_orbitCam.backZ * g_orbitCam.rightY;
    g_orbitCam.upY = g_orbitCam.backZ * g_orbitCam.rightX - g_orbitCam.backX * g_orbitCam.rightZ;
    g_orbitCam.upZ = g_orbitCam.backX * g_orbitCam.rightY - g_orbitCam.backY * g_orbitCam.rightX;

    // First-person pose: the pivot is the eye itself (the view direction is
    // still towards lookAt). Switching to orbit later backs off by `len`.
    g_orbitCam.targetX = scene.camPos[0];
    g_orbitCam.targetY = scene.camPos[1];
    g_orbitCam.targetZ = scene.camPos[2];
    g_orbitCam.distance = 0.0f;

    float ex = scene.boundsMax[0] - scene.boundsMin[0];
    float ey = scene.boundsMax[1] - scene.boundsMin[1];
    float ez = scene.boundsMax[2] - scene.boundsMin[2];
    const float diag = std::sqrt(ex * ex + ey * ey + ez * ez);
    g_orbitCam.nearPlane = 0.1f;
    g_orbitCam.farPlane = (len + diag) * 2.0f;
    g_orbitCam.minDistance = std::max(1.0f, diag * 0.002f);
    g_orbitCam.insideDistance = std::max(1.0f, diag * 0.05f);
    g_orbitCam.farDistance = len;
    g_orbitCam.insideView = false;
    g_orbitCam.firstPerson = true;
    g_orbitCam.fpMoveScale = std::max(0.05f, diag * 0.0008f);
    g_orbitCam.fpBackoff = len;
    g_orbitCam.pivotX = scene.camLookAt[0];
    g_orbitCam.pivotY = scene.camLookAt[1];
    g_orbitCam.pivotZ = scene.camLookAt[2];
    g_orbitCam.hasPivot = true;
    for (int i = 0; i < 3; i++) {
        const float margin = (scene.boundsMax[i] - scene.boundsMin[i]) * 0.15f;
        g_orbitCam.boundsMin[i] = scene.boundsMin[i] - margin;
        g_orbitCam.boundsMax[i] = scene.boundsMax[i] + margin;
    }
    g_orbitCam.hasBounds = true;
    g_orbitCam.LogState("gs-lod-initial");

    // The LumeGS render node uses the ECS scene camera. Keep its clip planes
    // and FOV in sync with the (scene-sized) LOD framing instead of the
    // small-model defaults (zFar = 100).
    if (g_lume.ecs && g_cameraEntity != CORE_NS::Entity {}) {
        if (auto* camMgr = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(*g_lume.ecs)) {
            if (auto cam = camMgr->Write(g_cameraEntity)) {
                cam->yFov = cfg.fovYDeg * BASE_NS::Math::DEG2RAD;
                cam->zNear = g_orbitCam.nearPlane;
                cam->zFar = g_orbitCam.farPlane;
            }
        }
    }
}

static void DestroyGsLodTiles()
{
    if (g_lume.ecs) {
        auto* tileSys = CORE_NS::GetSystem<GS::IGsTilingSystem>(*g_lume.ecs);
        for (auto& entity : g_gsLodTileEntities) {
            if (entity != CORE_NS::Entity {}) {
                if (tileSys) {
                    tileSys->DestroyTile(entity);
                }
                g_lume.ecs->GetEntityManager().Destroy(entity);
                entity = {};
            }
        }
    }
    g_gsLodTileEntities.clear();
    g_gsLodFineLevel.clear();
    g_gsLodPreload.clear();
    g_gsLodShownLevel.clear();
    g_gsLodTilePending.clear();
    g_gsLodVisibleOrder.clear();
    g_gsLodRingOrder.clear();
    g_gsLodPending.clear();
    g_gsLodPendingHead = 0;
    g_gsLodSelectionDirty = true;
    std::memset(g_gsLodLastView, 0, sizeof(g_gsLodLastView));
    std::memset(g_gsLodLastProj, 0, sizeof(g_gsLodLastProj));
    g_gsLodLastBudget = 0;
    g_gsLodLastResident = static_cast<size_t>(-1);
    g_gsLodScene = lod::Scene {};
    g_gsLodActive = false;

    // The single-model path resets isTiled/tilePool on the render data while
    // the tiling system keeps its queues/residency state. Cyclic the system
    // lifecycle here so a later LOD entry starts with a clean pool/queue.
    if (g_lume.ecs) {
        if (auto* tileSys = CORE_NS::GetSystem<GS::IGsTilingSystem>(*g_lume.ecs)) {
            tileSys->Uninitialize();
            tileSys->Initialize();
        }
    }
    LOGI("GS LOD: tiles destroyed");
}

static bool LoadGsLodScene(const DemoModelConfig& cfg)
{
    if (!g_lume.ecs || !cfg.scene) {
        return false;
    }
    const std::string dir = ResolveLodDir(cfg);
    LOGI("GS LOD scene dir: %{public}s", dir.c_str());
    lod::Scene scene;
    if (!scene.Load(dir) || scene.leaves.empty()) {
        LOGE("GS LOD: scene load failed");
        return false;
    }
    if (!EnsureGsConfiguration()) {
        LOGE("GS LOD: GS configuration missing");
        return false;
    }

    // Only drop the previous presentation after the metadata parsed, so a bad
    // scene keeps whatever was on screen.
    ReleaseSplatModel();
    DestroyGsLodTiles();
    ApplyGsLodCamera(scene, cfg);

    g_gsLodScene = BASE_NS::move(scene);
    const size_t leafCount = g_gsLodScene.leaves.size();
    g_gsLodTileEntities.assign(leafCount * 8u, CORE_NS::Entity {});
    g_gsLodFineLevel.assign(leafCount, -1);
    g_gsLodPreload.assign(leafCount, 0);
    g_gsLodShownLevel.assign(leafCount, -1);
    g_gsLodTilePending.assign(leafCount * 8u, 0);
    g_gsLodVisibleOrder.clear();
    g_gsLodRingOrder.clear();
    g_gsLodPending.clear();
    g_gsLodPendingHead = 0;
    g_gsLodSelectionDirty = true;
    std::memset(g_gsLodLastView, 0, sizeof(g_gsLodLastView));
    std::memset(g_gsLodLastProj, 0, sizeof(g_gsLodLastProj));
    g_gsLodLastBudget = 0;
    g_gsLodLastResident = static_cast<size_t>(-1);
    g_gsLodActive = true;
    g_forceRenderFrames = 30;
    LOGI("GS LOD: leaves=%{public}zu files=%{public}zu levels=%{public}d budget=%{public}u",
        g_gsLodScene.leaves.size(), g_gsLodScene.files.size(), g_gsLodScene.levelCount,
        g_lodBudgetPreference.load());
    return true;
}

// ---------------------------------------------------------------------------
// LumeGS tiled LOD host state machine (mirrors the system-side GSPlugin):
//   RebuildTileSelection
//   ProcessPendingTileCreates
//   ProcessVisibleTiles
//   ProcessPreloadTiles
//   cameraChanged/tileStateChanged/residencyChanged/unresolved -> RequestRender
// ---------------------------------------------------------------------------

static void EnqueueGsLodTile(uint32_t leafIndex, int level, bool preload)
{
    if (leafIndex >= g_gsLodScene.leaves.size() || level < 0) {
        return;
    }
    const lod::Leaf& leaf = g_gsLodScene.leaves[leafIndex];
    if (level >= leaf.levels) {
        return;
    }
    const lod::Slice& slice = leaf.lods[level];
    if (slice.file < 0 || slice.count == 0 ||
        static_cast<size_t>(slice.file) >= g_gsLodScene.files.size()) {
        return;
    }
    const size_t key = static_cast<size_t>(leafIndex) * 8u + static_cast<size_t>(level);
    if (g_gsLodTileEntities[key] != CORE_NS::Entity {} || g_gsLodTilePending[key] != 0) {
        return;
    }
    g_gsLodTilePending[key] = 1;
    g_gsLodPending.push_back({ leafIndex, level, preload });
}

// RebuildTileSelection: strict visible leaves/levels, plus a surrounding ring of
// coarsest-level preload tiles. Only runs when camera/projection/budget changed.
static bool RebuildGsLodSelection()
{
    if (!g_gsLodActive || !g_lume.ecs || g_gsLodScene.leaves.empty()) {
        return false;
    }
    const uint32_t budget = g_lodBudgetPreference.load();
    const bool cameraChanged =
        std::memcmp(g_gsLodLastView, g_gaussianCameraData.view, sizeof(g_gsLodLastView)) != 0 ||
        std::memcmp(g_gsLodLastProj, g_gaussianCameraData.proj, sizeof(g_gsLodLastProj)) != 0 ||
        budget != g_gsLodLastBudget;
    if (!cameraChanged && !g_gsLodSelectionDirty) {
        return false;
    }

    lod::Selection sel;
    lod::CullVisible(
        g_gsLodScene, g_gaussianCameraData.view, g_gaussianCameraData.proj, g_gaussianCameraData.cameraPos, sel);
    const float scale = lod::ChooseScale(g_gsLodScene, sel, budget);
    lod::AssignLevels(g_gsLodScene, sel, scale);

    const size_t leafCount = g_gsLodScene.leaves.size();
    g_gsLodFineLevel.assign(leafCount, -1);
    g_gsLodPreload.assign(leafCount, 0);
    g_gsLodVisibleOrder.clear();
    g_gsLodRingOrder.clear();
    std::vector<uint8_t> visible(leafCount, 0);
    for (const auto& item : sel.items) {
        if (item.leaf < leafCount) {
            g_gsLodFineLevel[item.leaf] = item.level;
            visible[item.leaf] = 1;
            g_gsLodVisibleOrder.push_back(item.leaf);
        }
    }

    float ringProj[16];
    std::memcpy(ringProj, g_gaussianCameraData.proj, sizeof(ringProj));
    ringProj[0] /= 1.3f;
    ringProj[5] /= 1.3f;
    lod::Selection ringSel;
    lod::CullVisible(
        g_gsLodScene, g_gaussianCameraData.view, ringProj, g_gaussianCameraData.cameraPos, ringSel);
    constexpr size_t kRingPreloadMax = 256;
    for (const auto& item : ringSel.items) {
        if (g_gsLodRingOrder.size() >= kRingPreloadMax) {
            break;
        }
        if (item.leaf >= leafCount || visible[item.leaf] != 0) {
            continue;
        }
        g_gsLodPreload[item.leaf] = 1;
        g_gsLodRingOrder.push_back(item.leaf);
    }

    // Nearest visible coarse fallback -> surrounding coarse ring -> remaining
    // coarse fallbacks -> visible fine levels.
    constexpr size_t kFastVisibleCoarse = 64;
    const size_t fastVisible =
        g_gsLodVisibleOrder.size() < kFastVisibleCoarse ? g_gsLodVisibleOrder.size() : kFastVisibleCoarse;
    for (size_t ii = 0; ii < fastVisible; ii++) {
        const uint32_t leafIndex = g_gsLodVisibleOrder[ii];
        EnqueueGsLodTile(leafIndex, g_gsLodScene.leaves[leafIndex].levels - 1, false);
    }
    for (const uint32_t leafIndex : g_gsLodRingOrder) {
        EnqueueGsLodTile(leafIndex, g_gsLodScene.leaves[leafIndex].levels - 1, true);
    }
    for (size_t ii = fastVisible; ii < g_gsLodVisibleOrder.size(); ii++) {
        const uint32_t leafIndex = g_gsLodVisibleOrder[ii];
        EnqueueGsLodTile(leafIndex, g_gsLodScene.leaves[leafIndex].levels - 1, false);
    }
    for (const uint32_t leafIndex : g_gsLodVisibleOrder) {
        const int fine = g_gsLodFineLevel[leafIndex];
        const int coarse = g_gsLodScene.leaves[leafIndex].levels - 1;
        if (fine != coarse) {
            EnqueueGsLodTile(leafIndex, fine, false);
        }
    }

    std::memcpy(g_gsLodLastView, g_gaussianCameraData.view, sizeof(g_gsLodLastView));
    std::memcpy(g_gsLodLastProj, g_gaussianCameraData.proj, sizeof(g_gsLodLastProj));
    g_gsLodLastBudget = budget;
    g_gsLodSelectionDirty = false;

    LOGI("GS LOD select: vis=%{public}d total=%{public}u scale=%{public}d ring=%{public}d pending=%{public}d",
        (int)g_gsLodVisibleOrder.size(), sel.total, (int)(scale * 1000.0f), (int)g_gsLodRingOrder.size(),
        (int)(g_gsLodPending.size() - g_gsLodPendingHead));
    return true;
}

// ProcessPendingTileCreates: bounded per-frame tile entity creation.
static bool ProcessPendingGsLodTileCreates(GS::IGsTilingSystem* tileSys)
{
    if (!tileSys || !g_lume.ecs) {
        return false;
    }
    auto& em = g_lume.ecs->GetEntityManager();
    constexpr size_t kCreatePerUpdate = 32;
    size_t created = 0;
    bool changed = false;
    while (g_gsLodPendingHead < g_gsLodPending.size() && created < kCreatePerUpdate) {
        const GsLodPendingCreate pending = g_gsLodPending[g_gsLodPendingHead++];
        const size_t key = static_cast<size_t>(pending.leaf) * 8u + static_cast<size_t>(pending.level);
        g_gsLodTilePending[key] = 0;
        if (g_gsLodTileEntities[key] != CORE_NS::Entity {}) {
            continue;
        }
        const lod::Leaf& leaf = g_gsLodScene.leaves[pending.leaf];
        if (pending.level < 0 || pending.level >= leaf.levels) {
            continue;
        }
        const lod::Slice& slice = leaf.lods[pending.level];
        if (slice.file < 0 || slice.count == 0 ||
            static_cast<size_t>(slice.file) >= g_gsLodScene.files.size()) {
            continue;
        }
        CORE_NS::Entity entity = em.Create();
        GS::GsTilingComponent comp;
        const std::string uri = std::string("file://") + g_gsLodScene.dir + "/" +
                                g_gsLodScene.files[static_cast<size_t>(slice.file)];
        comp.uri = BASE_NS::string(uri.c_str());
        comp.offset = slice.offset;
        comp.count = slice.count;
        comp.aabbMin = BASE_NS::Math::Vec3(leaf.bmin[0], leaf.bmin[1], leaf.bmin[2]);
        comp.aabbMax = BASE_NS::Math::Vec3(leaf.bmax[0], leaf.bmax[1], leaf.bmax[2]);
        comp.visible = false;
        comp.preload = pending.preload;
        tileSys->CreateTile(entity, comp);
        g_gsLodTileEntities[key] = entity;
        created++;
        changed = true;
    }
    if (g_gsLodPendingHead > 4096u && g_gsLodPendingHead * 2u > g_gsLodPending.size()) {
        g_gsLodPending.erase(
            g_gsLodPending.begin(), g_gsLodPending.begin() + static_cast<std::ptrdiff_t>(g_gsLodPendingHead));
        g_gsLodPendingHead = 0;
    }
    return changed;
}

// ProcessVisibleTiles: one visible level per visible leaf. Prefer the desired
// fine level when resident, otherwise the coarsest resident fallback; when
// neither exists yet show the fine tile (its load will request another frame).
static bool ProcessVisibleGsLodTiles(GS::IGsTilingSystem* tileSys, size_t& residentCount, bool& unresolved)
{
    if (!tileSys || !g_lume.ecs) {
        return false;
    }
    bool changed = false;
    for (size_t i = 0; i < g_gsLodScene.leaves.size(); i++) {
        const lod::Leaf& leaf = g_gsLodScene.leaves[i];
        const int fine = g_gsLodFineLevel[i];
        const int coarse = leaf.levels > 0 ? leaf.levels - 1 : -1;

        if (fine < 0) {
            const int shown = g_gsLodShownLevel[i];
            if (shown >= 0) {
                const auto entity = g_gsLodTileEntities[i * 8u + static_cast<size_t>(shown)];
                if (entity != CORE_NS::Entity {}) {
                    tileSys->SetTileVisible(entity, false);
                }
                g_gsLodShownLevel[i] = -1;
                changed = true;
            }
            continue;
        }

        int show = fine;
        bool found = false;
        for (int level = fine; level <= coarse; level++) {
            const auto entity = g_gsLodTileEntities[i * 8u + static_cast<size_t>(level)];
            if (entity != CORE_NS::Entity {} && tileSys->IsTileResident(entity)) {
                show = level;
                found = true;
                break;
            }
        }
        const auto fineEntity = g_gsLodTileEntities[i * 8u + static_cast<size_t>(fine)];
        const bool fineResident =
            fineEntity != CORE_NS::Entity {} && tileSys->IsTileResident(fineEntity);
        if (!fineResident) {
            unresolved = true;
        }
        if (!found) {
            // Nothing resident yet; keep/create the fine tile and let the
            // unresolved flag keep frames coming.
            show = fine;
        }

        const auto entity = g_gsLodTileEntities[i * 8u + static_cast<size_t>(show)];
        if (entity != CORE_NS::Entity {}) {
            if (g_gsLodShownLevel[i] != show) {
                const int old = g_gsLodShownLevel[i];
                if (old >= 0) {
                    const auto oldEntity = g_gsLodTileEntities[i * 8u + static_cast<size_t>(old)];
                    if (oldEntity != CORE_NS::Entity {}) {
                        tileSys->SetTileVisible(oldEntity, false);
                    }
                }
                tileSys->SetTileVisible(entity, true);
                g_gsLodShownLevel[i] = show;
                changed = true;
            }
            if (tileSys->IsTileResident(entity)) {
                residentCount++;
            }
        }
    }
    return changed;
}

// ProcessPreloadTiles: the surrounding ring stays invisible and preloaded.
static bool ProcessPreloadGsLodTiles(GS::IGsTilingSystem* tileSys, size_t& residentCount, bool& unresolved)
{
    if (!tileSys) {
        return false;
    }
    bool changed = false;
    for (const uint32_t leafIndex : g_gsLodRingOrder) {
        if (leafIndex >= g_gsLodScene.leaves.size() || g_gsLodPreload[leafIndex] == 0 ||
            g_gsLodFineLevel[leafIndex] >= 0) {
            continue;
        }
        const lod::Leaf& leaf = g_gsLodScene.leaves[leafIndex];
        const int coarse = leaf.levels > 0 ? leaf.levels - 1 : -1;
        if (coarse < 0) {
            continue;
        }
        const auto entity = g_gsLodTileEntities[leafIndex * 8u + static_cast<size_t>(coarse)];
        if (entity == CORE_NS::Entity {}) {
            unresolved = true;
            continue;
        }
        if (tileSys->IsTileResident(entity)) {
            residentCount++;
        } else {
            unresolved = true;
        }
        // Preload tiles must stay invisible.
        const int shown = g_gsLodShownLevel[leafIndex];
        if (shown >= 0) {
            const auto shownEntity = g_gsLodTileEntities[leafIndex * 8u + static_cast<size_t>(shown)];
            if (shownEntity != CORE_NS::Entity {}) {
                tileSys->SetTileVisible(shownEntity, false);
            }
            g_gsLodShownLevel[leafIndex] = -1;
            changed = true;
        }
    }
    return changed;
}

static void UpdateGsLodTiles()
{
    if (!g_gsLodActive || !g_lume.ecs || g_gsLodScene.leaves.empty()) {
        return;
    }
    auto* tileSys = CORE_NS::GetSystem<GS::IGsTilingSystem>(*g_lume.ecs);
    if (!tileSys) {
        return;
    }

    const bool selectionChanged = RebuildGsLodSelection();
    const bool tilesCreated = ProcessPendingGsLodTileCreates(tileSys);

    size_t residentCount = 0;
    bool unresolvedVisible = false;
    const bool visibleChanged = ProcessVisibleGsLodTiles(tileSys, residentCount, unresolvedVisible);

    bool unresolvedPreload = false;
    const bool preloadChanged = ProcessPreloadGsLodTiles(tileSys, residentCount, unresolvedPreload);

    const bool residencyChanged = residentCount != g_gsLodLastResident;
    g_gsLodLastResident = residentCount;

    const bool needRender = selectionChanged || tilesCreated || visibleChanged || preloadChanged ||
                            residencyChanged || unresolvedVisible || unresolvedPreload;
    if (needRender) {
        // Same trigger as the system host: a field change (or an unresolved tile)
        // requests exactly the next frame.
        g_lume.ecs->RequestRender();
        g_forceRenderFrames = 3;
    }

    if (selectionChanged || visibleChanged) {
        LOGI("GS LOD state: vis=%{public}d ring=%{public}d res=%{public}d unresolved=%d/%d pending=%{public}d",
            (int)g_gsLodVisibleOrder.size(), (int)g_gsLodRingOrder.size(), (int)residentCount,
            unresolvedVisible ? 1 : 0, unresolvedPreload ? 1 : 0,
            (int)(g_gsLodPending.size() - g_gsLodPendingHead));
    }
}

struct SplatDecodeTask {
    std::string path;
    DemoModelConfig cfg;
    uint32_t generation = 0;
};

static std::thread g_splatDecodeThread;
static std::atomic<bool> g_splatDecodeDone { false };
static std::atomic<bool> g_splatDecodeOk { false };
static SplatData g_splatDecodeData;
static SplatDecodeTask g_splatDecodeTask;
static SplatDecodeTask g_splatDecodeQueuedTask;

static bool DecodeSplatFile(const SplatDecodeTask& task, SplatData& out)
{
    if (strstr(task.cfg.file, ".glb") != nullptr) {
        return LoadUwaSplatGlb(task.path.c_str(), out);
    }
    if (strstr(task.cfg.file, ".sog") != nullptr) {
        // SH rest costs 48 floats per gaussian; only decode it when the model
        // configuration actually evaluates higher bands.
        return sog::LoadSogFile(task.path.c_str(), out, 100.0f,
            task.cfg.shDeg1 > 0.0f || task.cfg.shDeg23 > 0.0f);
    }
    if (strstr(task.cfg.file, ".splat") != nullptr) {
        return LoadSplatFile(task.path.c_str(), out);
    }
    return LoadStandard3DgsPly(task.path.c_str(), out, 100.0f);
}

static void StartSplatDecode(const SplatDecodeTask& task)
{
    g_splatDecodeTask = task;
    g_splatDecodeDone.store(false, std::memory_order_relaxed);
    g_splatDecodeOk.store(false, std::memory_order_relaxed);
    g_splatDecodeData = SplatData {};
    g_splatDecodeThread = std::thread([task]() {
        SplatData data;
        const bool ok = DecodeSplatFile(task, data);
        g_splatDecodeData = std::move(data);
        g_splatDecodeOk.store(ok, std::memory_order_relaxed);
        g_splatDecodeDone.store(true, std::memory_order_release);
    });
}

static void ApplySplatModelData(const SplatDecodeTask& task);

static void PollSplatDecode()
{
    if (!g_splatDecodeThread.joinable()) {
        if (g_splatDecodeQueued) {
            g_splatDecodeQueued = false;
            StartSplatDecode(g_splatDecodeQueuedTask);
        }
        return;
    }
    if (!g_splatDecodeDone.load(std::memory_order_acquire)) {
        return;
    }
    g_splatDecodeThread.join();

    const bool stale = g_splatDecodeTask.generation != g_splatDecodeGeneration;
    if (!stale && g_splatDecodeOk.load(std::memory_order_relaxed)) {
        g_lume.splatData = std::move(g_splatDecodeData);
        g_splatDataInUsePath = g_splatDecodeTask.path;
        ApplySplatModelData(g_splatDecodeTask);
    } else if (!stale) {
        LOGE("Failed to load splat model: %{public}s", g_splatDecodeTask.path.c_str());
    }
    g_splatDecodeData = SplatData {};

    if (g_splatDecodeQueued) {
        g_splatDecodeQueued = false;
        StartSplatDecode(g_splatDecodeQueuedTask);
    }
}

static void StopSplatDecode()
{
    if (g_splatDecodeThread.joinable()) {
        g_splatDecodeThread.join();
    }
    g_splatDecodeQueued = false;
    g_splatDecodeData = SplatData {};
}

void LoadSplatModel()
{
    ReleaseSplatModel();

    const DemoModelConfig& cfg = kDemoModels[g_modelIndex];
    if (cfg.scene != nullptr) {
        LoadLodScene(cfg);
        return;
    }
    SetClearColor(0.006f, 0.004f, 0.002f);  // warm black (matches the web viewer)
    g_gaussianBackground[0] = 0.071f;
    g_gaussianBackground[1] = 0.047f;
    g_gaussianBackground[2] = 0.024f;
    g_gaussianBackground[3] = 1.0f;
    const std::string path = g_filesDir + "/assets/" + cfg.file;
    LOGI("Loading splat model: %{public}s", path.c_str());

    ++g_splatDecodeGeneration;
    g_splatDecodeQueued = false;

    const bool fromCache = !g_cachedSplatPath.empty() && g_cachedSplatPath == path &&
                           !g_cachedSplatData.gaussians.empty();
    if (fromCache) {
        g_lume.splatData = std::move(g_cachedSplatData);
        g_cachedSplatData = SplatData {};
        g_splatDataInUsePath = path;
        LOGI("Splat cache: %{public}u gaussians", g_lume.splatData.gaussianCount);
        ApplySplatModelData(SplatDecodeTask { path, cfg, g_splatDecodeGeneration });
        return;
    }

    SplatDecodeTask task { path, cfg, g_splatDecodeGeneration };
    if (g_splatDecodeThread.joinable()) {
        // Keep the render thread responsive: let the current worker finish, then
        // start the latest request (the stale result is discarded by generation).
        g_splatDecodeQueuedTask = task;
        g_splatDecodeQueued = true;
    } else {
        StartSplatDecode(task);
    }
}

static void ApplySplatModelData(const SplatDecodeTask& task)
{
    const DemoModelConfig& cfg = task.cfg;

    g_lume.splatLoaded = true;
    LOGI("Splat loaded: %{public}u gaussians, bounds=(%{public}.2f,%{public}.2f,%{public}.2f)-(%{public}.2f,%{public}.2f,%{public}.2f)",
        g_lume.splatData.gaussianCount,
        g_lume.splatData.boundsMin[0], g_lume.splatData.boundsMin[1], g_lume.splatData.boundsMin[2],
        g_lume.splatData.boundsMax[0], g_lume.splatData.boundsMax[1], g_lume.splatData.boundsMax[2]);
    for (uint32_t i = 0; i < 3 && i < g_lume.splatData.gaussianCount; i++) {
        auto& g = g_lume.splatData.gaussians[i];
        LOGI("  g[%{public}u] pos=(%{public}.3f,%{public}.3f,%{public}.3f) scale=(%{public}.4f,%{public}.4f,%{public}.4f) color=(%{public}.2f,%{public}.2f,%{public}.2f,%{public}.2f)",
            i, g.px, g.py, g.pz, g.sx, g.sy, g.sz, g.cr, g.cg, g.cb, g.opacity);
    }

    // ---- Per-model rendering configuration ----
    g_orbitCam.fovYDeg = cfg.fovYDeg;
    g_orbitCam.shDeg1 = cfg.shDeg1;
    g_orbitCam.shDeg23 = cfg.shDeg23;

    // ---- Framing: start close to the content; double-tap toggles between
    // this inside view and the full far framing. Models with a recorded
    // viewpoint keep its orientation; the orbit target and distances come from
    // robust statistics so outliers (walls, env spheres) do not swallow the view.
    const SplatFraming framing = ComputeSplatFraming(g_lume.splatData);
    const float ex = g_lume.splatData.boundsMax[0] - g_lume.splatData.boundsMin[0];
    const float ey = g_lume.splatData.boundsMax[1] - g_lume.splatData.boundsMin[1];
    const float ez = g_lume.splatData.boundsMax[2] - g_lume.splatData.boundsMin[2];
    float maxExtent = ex > ey ? (ex > ez ? ex : ez) : (ey > ez ? ey : ez);
    if (maxExtent < 0.01f) maxExtent = 100.0f;
    const float subjectRadius = (framing.r80 > 0.01f) ? framing.r80 : maxExtent * 0.5f;
    LOGI("Framing: center=(%{public}.2f,%{public}.2f,%{public}.2f) r50=%{public}.2f r80=%{public}.2f r90=%{public}.2f",
        framing.cx, framing.cy, framing.cz, framing.r50, framing.r80, framing.r90);

    if (cfg.hasView) {
        g_orbitCam.rightX = cfg.right[0]; g_orbitCam.rightY = cfg.right[1]; g_orbitCam.rightZ = cfg.right[2];
        g_orbitCam.upX = cfg.up[0]; g_orbitCam.upY = cfg.up[1]; g_orbitCam.upZ = cfg.up[2];
        g_orbitCam.backX = cfg.back[0]; g_orbitCam.backY = cfg.back[1]; g_orbitCam.backZ = cfg.back[2];
    } else {
        g_orbitCam.rightX = 1.f; g_orbitCam.rightY = 0.f; g_orbitCam.rightZ = 0.f;
        g_orbitCam.upX = 0.f; g_orbitCam.upY = 1.f; g_orbitCam.upZ = 0.f;
        g_orbitCam.backX = 0.f; g_orbitCam.backY = 0.f; g_orbitCam.backZ = 1.f;
    }

    // Orbit around the subject. A recorded target is only used when the caller
    // asked for one explicitly (all-zero target means "use the centroid").
    const bool explicitTarget = (cfg.target[0] != 0.f || cfg.target[1] != 0.f || cfg.target[2] != 0.f);
    if (cfg.hasView && explicitTarget) {
        g_orbitCam.targetX = cfg.target[0];
        g_orbitCam.targetY = cfg.target[1];
        g_orbitCam.targetZ = cfg.target[2];
    } else {
        g_orbitCam.targetX = framing.cx;
        g_orbitCam.targetY = framing.cy;
        g_orbitCam.targetZ = framing.cz;
    }

    g_orbitCam.firstPerson = false;
    g_orbitCam.fpBackoff = 0.0f;
    g_orbitCam.hasPivot = false;
    g_orbitCam.hasBounds = false;
    // Restore the small-scene clip range (the LOD scene widens it for the scene
    // diagonal, and leaving it wide changes which splats survive the far cull).
    g_orbitCam.nearPlane = 0.05f;
    g_orbitCam.farPlane = 2000.0f;
    g_orbitCam.minDistance = 0.1f;
    g_orbitCam.farDistance = (cfg.farDistance > 0.f) ? cfg.farDistance : subjectRadius * 4.0f;
    g_orbitCam.insideDistance = (cfg.insideDistance > 0.f) ? cfg.insideDistance : subjectRadius * 1.6f;
    if (g_orbitCam.insideDistance < 0.5f) g_orbitCam.insideDistance = 0.5f;
    g_orbitCam.distance = g_orbitCam.insideDistance;
    g_orbitCam.insideView = true;
    g_orbitCam.LogState("initial");

    // Feed data to the RenderNode via shared globals
    static_assert(sizeof(GaussianSplatGPU) == sizeof(GpuGaussianData),
        "GaussianSplatGPU must match GpuGaussianData layout");
    g_gaussianPoolMode = 0;  // monolithic array: 0 dirty ranges = full upload
    g_gaussianSplatData = reinterpret_cast<const GpuGaussianData*>(g_lume.splatData.gaussians.data());
    g_gaussianSplatCount = g_lume.splatData.gaussianCount;
    g_gaussianSplatCapacity = g_lume.splatData.gaussianCount;
    g_gaussianPoolSize = g_lume.splatData.gaussianCount;
    g_gaussianLiveRanges = nullptr;
    g_gaussianLiveRangeCount = 0;
    g_gaussianDirtyRanges = nullptr;  // full upload
    g_gaussianDirtyRangeCount = 0;
    g_gaussianShData = g_lume.splatData.sh.empty() ? nullptr : g_lume.splatData.sh.data();
    g_gaussianDataReady = true;
    ++g_gaussianSplatGeneration;

    // Create the RNG for the gaussian splatting node
    auto& rngMgr = g_lume.renderContext->GetRenderNodeGraphManager();
    RENDER_NS::RenderNodeGraphDesc rngDesc {};
    rngDesc.renderNodeGraphName = "GaussianSplatRNG";

    RENDER_NS::RenderNodeDesc nodeDesc {};
    nodeDesc.typeName = "RenderNodeGaussianSplatting";
    nodeDesc.nodeName = "GaussianSplatNode";
    rngDesc.nodes.push_back(BASE_NS::move(nodeDesc));

    g_lume.splatRngHandle = rngMgr.Create(
        RENDER_NS::IRenderNodeGraphManager::RenderNodeGraphUsageType::RENDER_NODE_GRAPH_STATIC,
        rngDesc);
    LOGI("Gaussian splat RNG created: valid=%{public}d", g_lume.splatRngHandle ? 1 : 0);

    if (g_pendingCameraMode >= 0) {
        SetFirstPerson(g_pendingCameraMode != 0);
        g_pendingCameraMode = -1;
    }

    // Show the newly decoded model even when the app is otherwise idle.
    g_forceRenderFrames = 8;
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void DrawFrame()
{
    if (!g_lume.ready || !g_lume.engine) {
        return;
    }

    PollSplatDecode();

    // Apply a pending model switch requested from ArkTS (main thread -> here,
    // so the RNG/GPU resources are created and released on one thread only).
    if (g_pendingModelIndex >= 0 && g_pendingModelIndex < kDemoModelCount &&
        g_pendingModelIndex != g_modelIndex) {
        g_modelIndex = g_pendingModelIndex;
        const bool wantsLod = kDemoModels[g_modelIndex].scene != nullptr;
        if (wantsLod) {
            // Parse metadata and set up the LumeGS tiling path first. Only hide
            // the previous presentation after success, so a malformed
            // default.scene.json keeps the current model instead of going black.
            if (LoadGsLodScene(kDemoModels[g_modelIndex])) {
                g_renderPath = 1;
                DestroyGsRenderPath();
            } else {
                LOGE("LOD scene failed to start; keeping previous model");
                g_renderPath = 1;
            }
        } else {
            g_renderPath = 1;
            if (g_gsLodActive) {
                DestroyGsLodTiles();
            }
            g_gsModelFile = kDemoModels[g_modelIndex].file;
            if (g_gsModelEntity == CORE_NS::Entity {}) {
                // PollGsRenderPath will create the node for this file.
                g_gsModelDirty = false;
            } else {
                g_gsModelDirty = true;
            }
            LoadSplatModel();
        }
        // Keep the warmup short; 30 frames kept the main thread busy long
        // enough to delay the first touch after a switch.
        g_forceRenderFrames = 8;
    }
    g_pendingModelIndex = -1;

    // Update shared camera data for the gaussian splatting RenderNode
    if (g_lume.width > 0 && g_lume.height > 0) {
        g_orbitCam.UpdateSharedCameraData((uint32_t)g_lume.width, (uint32_t)g_lume.height);
    }

    // LumeGS tiled LOD: pick per-leaf levels and toggle tile visibility. The
    // GsTilingSystem worker handles SOG decode / pool residency.
    if (g_gsLodActive) {
        UpdateGsLodTiles();
    }

    // LOD scene: feed the streamer when the view changed and pick up newly
    // assembled data. The worker decodes tiles in the background, so the first
    // frames render whatever is already available.
    if (g_lodActive) {
        if (std::memcmp(g_lastLodView, g_gaussianCameraData.view, sizeof(g_lastLodView)) != 0) {
            std::memcpy(g_lastLodView, g_gaussianCameraData.view, sizeof(g_lastLodView));
            g_lodStreamer.UpdateCamera(g_gaussianCameraData.view, g_gaussianCameraData.proj,
                g_gaussianCameraData.cameraPos);
        }
        lod::Streamer::Result lod {};
        if (g_lodStreamer.Poll(lod)) {
            g_gaussianSplatData = lod.data;
            g_gaussianSplatCount = lod.count;
            g_gaussianSplatCapacity = g_lodStreamer.Capacity();
            g_gaussianShData = nullptr;
            g_gaussianDataReady = true;
            g_gaussianPoolSize = lod.poolSize;
            g_gaussianLiveRanges = lod.liveRanges;
            g_gaussianLiveRangeCount = lod.liveRangeCount;
            g_gaussianDirtyRanges = lod.dirtyRanges;
            g_gaussianDirtyRangeCount = lod.dirtyRangeCount;
            ++g_gaussianSplatGeneration;
            g_forceRenderFrames = 30;
            LOGI("LOD: published %{public}u/%{public}u gaussians (gen 0x%{public}x)", lod.count,
                lod.poolSize, lod.generation);
        }
    }

    PollGsRenderPath();

    // LumeGS renders with the scene main camera, not with the custom node's
    // shared camera data: keep the scene camera in sync with the orbit camera.
    if (g_renderPath == 1 && g_lume.ecs && g_cameraEntity != CORE_NS::Entity {} &&
        g_lume.graphicsContext) {
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        g_orbitCam.GetPosition(ex, ey, ez);
        // In first person the eye IS the pivot (target == eye), so looking at
        // the orbit target would produce a zero-length look direction and a
        // degenerate view matrix. Look forward along -back instead.
        float tx = g_orbitCam.targetX;
        float ty = g_orbitCam.targetY;
        float tz = g_orbitCam.targetZ;
        if (g_orbitCam.firstPerson) {
            constexpr float kLookAhead = 10.0f;
            tx = ex - g_orbitCam.backX * kLookAhead;
            ty = ey - g_orbitCam.backY * kLookAhead;
            tz = ez - g_orbitCam.backZ * kLookAhead;
        }
        g_lume.graphicsContext->GetSceneUtil().CameraLookAt(*g_lume.ecs, g_cameraEntity,
            { ex, ey, ez },
            { tx, ty, tz },
            { g_orbitCam.upX, g_orbitCam.upY, g_orbitCam.upZ });
    }

    // GsSystem creates the splat render targets from the main camera's
    // renderResolution. The camera is created during engine init, when the
    // surface size is still unknown, so refresh it once the real size is known
    // (and on resize).
    if (g_renderPath == 1 && g_lume.ecs && g_cameraEntity != CORE_NS::Entity {} &&
        g_lume.width > 0 && g_lume.height > 0) {
        static uint32_t lastW = 0;
        static uint32_t lastH = 0;
        if (lastW != (uint32_t)g_lume.width || lastH != (uint32_t)g_lume.height) {
            lastW = (uint32_t)g_lume.width;
            lastH = (uint32_t)g_lume.height;
            g_lume.graphicsContext->GetSceneUtil().UpdateCameraViewport(
                *g_lume.ecs, g_cameraEntity, { lastW, lastH });
            // GsSystem reads CameraComponent::renderResolution (not the viewport)
            // to size the splat render targets; set it explicitly.
            if (auto* camMgr = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(*g_lume.ecs)) {
                if (auto camHandle = camMgr->Write(g_cameraEntity)) {
                    camHandle->renderResolution = { lastW, lastH };
                }
            }
            LOGI("GS path: camera render resolution %{public}u x %{public}u", lastW, lastH);
        }
    }

    // TickFrame updates internal time and drives all ECS systems.
    CORE_NS::IEcs* ecsArr[] = { g_lume.ecs.get() };
    bool needsRender = g_lume.engine->TickFrame(
        BASE_NS::array_view<CORE_NS::IEcs*>(ecsArr, 1u));

    // Render-on-demand: skip the expensive RenderFrame when the view is static.
    // This keeps the GPU idle (and the device cool) between interactions.
    bool viewChanged = (std::memcmp(g_lastRenderedView, g_gaussianCameraData.view,
                            sizeof(g_lastRenderedView)) != 0);
    if (g_forceRenderFrames > 0) {
        g_forceRenderFrames--;
    }
    const bool doRender = viewChanged || (g_forceRenderFrames > 0) || (g_lume.frameCount < 5);

    if (doRender && needsRender && g_lume.graphicsContext && g_lume.renderer) {
        std::memcpy(g_lastRenderedView, g_gaussianCameraData.view, sizeof(g_lastRenderedView));

        auto rngs = g_lume.graphicsContext->GetRenderNodeGraphs(*g_lume.ecs);

        // Append gaussian splatting RNGs (legacy custom node and/or LumeGS node)
        if (g_lume.splatRngHandle || g_gsRngHandle) {
            // Build a combined list: scene RNGs + splat RNG
            BASE_NS::vector<RENDER_NS::RenderHandleReference> allRngs;
            allRngs.reserve(rngs.size() + 2);
            for (size_t i = 0; i < rngs.size(); i++) {
                allRngs.push_back(rngs[i]);
            }
            if (g_lume.splatRngHandle) {
                allRngs.push_back(g_lume.splatRngHandle);
            }
            if (g_gsRngHandle) {
                allRngs.push_back(g_gsRngHandle);
            }
            g_lume.renderer->RenderFrame(
                BASE_NS::array_view<const RENDER_NS::RenderHandleReference>(allRngs.data(), allRngs.size()));
        } else {
            g_lume.renderer->RenderFrame(rngs);
        }
    }

    g_lume.frameCount++;
    if (g_lume.frameCount <= 5 || (g_lume.frameCount % 300 == 0)) {
        LOGI("Tick %{public}llu (rendered=%{public}d)",
            (unsigned long long)g_lume.frameCount, doRender ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void DestroyEngine()
{
    LOGI("Destroying engine resources...");
    StopSplatDecode();
    g_lume.ready = false;

    // Destroy in reverse init order.
    if (g_lume.ecs) {
        g_lume.ecs->Uninitialize();
        g_lume.ecs->ProcessEvents();
    }
    g_lume.ecs.reset();

    g_lume.graphicsContext.reset();

    if (g_lume.device) {
        g_lume.device->DestroySwapchain();
    }

    g_lume.renderer = nullptr;
    g_lume.device = nullptr;
    g_lume.renderContext.reset();
    g_lume.engine.reset();
    g_lume.nativeWindow = nullptr;
    g_lume.frameCount = 0;

    LOGI("Engine resources destroyed");
}

// ---------------------------------------------------------------------------
// XComponent callbacks
// ---------------------------------------------------------------------------

void OnSurfaceCreated(OH_NativeXComponent* component, void* window)
{
    LOGI("OnSurfaceCreated (tid=%{public}d)", gettid());
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    LOGI("Surface size: %{public}lux%{public}lu", (unsigned long)w, (unsigned long)h);

    g_lume.nativeWindow = static_cast<OHNativeWindow*>(window);
    g_lume.width = w;
    g_lume.height = h;

    // Set buffer geometry so the native window allocates the right size.
    OH_NativeWindow_NativeWindowHandleOpt(
        g_lume.nativeWindow, SET_BUFFER_GEOMETRY,
        static_cast<int32_t>(w), static_cast<int32_t>(h));

    // Full initialization sequence.
    bool ok = InitPluginRegistry();
    if (ok) ok = InitEngine();
    if (ok) ok = InitRenderContext();  // creates device, swapchain, then Init
    if (ok) ok = InitGraphicsContext();
    if (ok) ok = InitEcs();

    if (ok) {
        g_lume.ready = true;
        LOGI("=== Lume engine fully initialized ===");

        // Set up scene: camera, light.
        SetupCameraAndLight();

        // CRITICAL: Tick one frame BEFORE loading the model. This lets RenderSystem::Initialize()
        // run and register its event listeners. If we load the model before the first tick,
        // RenderSystem's meshGeneration_ counter absorbs the mesh creation and the generation
        // check never detects it as new, leaving meshModifiedEvents_ permanently empty.
        DrawFrame();
        LOGI("First tick completed — RenderSystem initialized, now loading model");


        // Load 3DGS splat data (if available)
        LoadSplatModel();

        // Render second frame to process the newly loaded model.
        DrawFrame();
    } else {
        LOGE("=== Lume engine initialization FAILED ===");
    }
}

void OnSurfaceChanged(OH_NativeXComponent* component, void* window)
{
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(component, window, &w, &h);
    LOGI("OnSurfaceChanged %{public}lux%{public}lu", (unsigned long)w, (unsigned long)h);
    g_lume.width = w;
    g_lume.height = h;
    OH_NativeWindow_NativeWindowHandleOpt(
        g_lume.nativeWindow, SET_BUFFER_GEOMETRY,
        static_cast<int32_t>(w), static_cast<int32_t>(h));
}

void OnSurfaceDestroyed(OH_NativeXComponent* /*component*/, void* /*window*/)
{
    LOGI("OnSurfaceDestroyed");
    DestroyEngine();
}

void DispatchTouchEvent(OH_NativeXComponent* component, void* window)
{
    OH_NativeXComponent_TouchEvent touchEvent;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &touchEvent) != 0) {
        DrawFrame();
        return;
    }

    const float sensitivity = 0.005f;  // radians per pixel
    const float zoomSensitivity = 0.01f;

    float x = touchEvent.x;
    float y = touchEvent.y;

    switch (touchEvent.type) {
    case OH_NATIVEXCOMPONENT_DOWN: {
        if (touchEvent.numPoints == 1) {
            g_orbitCam.lastX = x;
            g_orbitCam.lastY = y;
            g_orbitCam.dragging = true;
            g_orbitCam.pinching = false;
        } else if (touchEvent.numPoints >= 2) {
            // Start pinch
            g_orbitCam.dragging = false;
            g_orbitCam.pinching = true;
            float x1 = touchEvent.touchPoints[0].x;
            float y1 = touchEvent.touchPoints[0].y;
            float x2 = touchEvent.touchPoints[1].x;
            float y2 = touchEvent.touchPoints[1].y;
            float dx = x2 - x1, dy = y2 - y1;
            g_orbitCam.lastDist = sqrtf(dx*dx + dy*dy);
            // Also store for pan
            g_orbitCam.lastX = (x1 + x2) * 0.5f;
            g_orbitCam.lastY = (y1 + y2) * 0.5f;
        }
        break;
    }
    case OH_NATIVEXCOMPONENT_MOVE: {
        // Keep rendering for a couple frames after the last touch event
        g_forceRenderFrames = 3;
        if (touchEvent.numPoints >= 2 && g_orbitCam.pinching) {
            // Pinch zoom
            float x1 = touchEvent.touchPoints[0].x;
            float y1 = touchEvent.touchPoints[0].y;
            float x2 = touchEvent.touchPoints[1].x;
            float y2 = touchEvent.touchPoints[1].y;
            float dx = x2 - x1, dy = y2 - y1;
            float dist = sqrtf(dx*dx + dy*dy);
            if (g_orbitCam.lastDist > 1.f) {
                if (g_orbitCam.firstPerson) {
                    // Pinch dollies along the view direction instead of zooming.
                    g_orbitCam.Dolly((dist - g_orbitCam.lastDist) * g_orbitCam.fpMoveScale);
                } else {
                    g_orbitCam.distance *= g_orbitCam.lastDist / dist;
                    if (g_orbitCam.distance < g_orbitCam.minDistance) {
                        g_orbitCam.distance = g_orbitCam.minDistance;
                    }
                    // Keep the target comfortably inside the far plane, otherwise
                    // the whole scene can end up beyond it at maximum zoom-out.
                    const float maxDistance = g_orbitCam.farPlane * 0.75f;
                    if (g_orbitCam.distance > maxDistance) {
                        g_orbitCam.distance = maxDistance;
                    }
                }
            }
            g_orbitCam.lastDist = dist;

            // Two-finger pan
            float mx = (x1 + x2) * 0.5f;
            float my = (y1 + y2) * 0.5f;
            float panDx = mx - g_orbitCam.lastX;
            float panDy = my - g_orbitCam.lastY;
            if (g_orbitCam.firstPerson) {
                // Two-finger drag walks: horizontal strafes, vertical dollies.
                g_orbitCam.Walk(panDx, panDy, g_orbitCam.fpMoveScale);
            } else {
                float panScale = g_orbitCam.distance * 0.002f;
                g_orbitCam.Pan(panDx, panDy, panScale);
            }
            g_orbitCam.lastX = mx;
            g_orbitCam.lastY = my;
        } else if (touchEvent.numPoints == 1 && g_orbitCam.dragging) {
            // Free tumble — no angle limits
            float dx = x - g_orbitCam.lastX;
            float dy = y - g_orbitCam.lastY;
            g_orbitCam.Tumble(-dx * sensitivity, -dy * sensitivity);
            g_orbitCam.lastX = x;
            g_orbitCam.lastY = y;
        }
        break;
    }
    case OH_NATIVEXCOMPONENT_UP:
    case OH_NATIVEXCOMPONENT_CANCEL:
        g_orbitCam.dragging = false;
        g_orbitCam.pinching = false;
        // Double-tap: toggle between the far framing and the model centre
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double nowMs = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
            const float dx = x - g_orbitCam.lastTapX;
            const float dy = y - g_orbitCam.lastTapY;
            if (touchEvent.type == OH_NATIVEXCOMPONENT_UP &&
                nowMs - g_orbitCam.lastTapTimeMs < 350.0 &&
                dx*dx + dy*dy < 1600.0f) {
                if (g_orbitCam.firstPerson) {
                    g_orbitCam.lastTapTimeMs = 0.0;
                } else if (g_orbitCam.insideView) {
                    g_orbitCam.distance = g_orbitCam.farDistance;
                    g_orbitCam.insideView = false;
                    LOGI("Camera: far view (distance %{public}.2f)", g_orbitCam.distance);
                } else {
                    g_orbitCam.distance = g_orbitCam.insideDistance;
                    g_orbitCam.insideView = true;
                    LOGI("Camera: centre view (distance %{public}.2f)", g_orbitCam.distance);
                }
                g_orbitCam.lastTapTimeMs = 0.0;
            } else if (touchEvent.type == OH_NATIVEXCOMPONENT_UP) {
                g_orbitCam.lastTapTimeMs = nowMs;
                g_orbitCam.lastTapX = x;
                g_orbitCam.lastTapY = y;
            }
        }
        g_orbitCam.LogState("after-touch");
        break;
    default:
        break;
    }

    // Update shared camera data for RenderNode
    if (g_lume.width > 0 && g_lume.height > 0) {
        g_orbitCam.UpdateSharedCameraData((uint32_t)g_lume.width, (uint32_t)g_lume.height);
    }

    DrawFrame();
}

// ---------------------------------------------------------------------------
// NAPI module — register XComponent callbacks + drawFrame export
// ---------------------------------------------------------------------------

static napi_value NapiInitResourceManager(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc >= 1 && args[0] != nullptr) {
        g_resMan = OH_ResourceManager_InitNativeResourceManager(env, args[0]);
        LOGI("NativeResourceManager initialized: %{public}p", g_resMan);
    }
    if (argc >= 2 && args[1] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &len);
        g_filesDir.resize(len);
        napi_get_value_string_utf8(env, args[1], &g_filesDir[0], len + 1, &len);
        LOGI("filesDir: %{public}s", g_filesDir.c_str());
    }
    return nullptr;
}

static napi_value NapiDrawFrame(napi_env /*env*/, napi_callback_info /*info*/)
{
    DrawFrame();
    return nullptr;
}

static napi_value NapiSetCameraMode(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        return nullptr;
    }
    bool firstPerson = false;
    napi_get_value_bool(env, args[0], &firstPerson);
    // Models are loaded on the render thread from a pending request; if one is
    // in flight the mode is applied right after the new camera is set up.
    g_pendingCameraMode = firstPerson ? 1 : 0;
    if (g_pendingModelIndex < 0) {
        SetFirstPerson(firstPerson);
        g_pendingCameraMode = -1;
    }
    return nullptr;
}

static napi_value NapiSetLodBudget(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t value = 0;
    if (argc >= 1 && args[0] != nullptr && napi_get_value_int32(env, args[0], &value) == napi_ok &&
        value > 0) {
        g_lodBudgetPreference.store((uint32_t)value);
        if (g_lodActive) {
            g_lodStreamer.SetBudget((uint32_t)value);
        }
        LOGI("setLodBudget: %{public}d", value);
    }
    return nullptr;
}

static napi_value NapiSetModel(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1 || args[0] == nullptr) {
        return nullptr;
    }

    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    std::string name(len, '\0');
    napi_get_value_string_utf8(env, args[0], &name[0], len + 1, &len);

    for (int i = 0; i < kDemoModelCount; i++) {
        std::string file = kDemoModels[i].file;
        if (kDemoModels[i].scene != nullptr) {
            file = SceneBaseName(kDemoModels[i].scene);
        }
        // Accept either the full file name ("doll.glb") or the short name ("doll").
        if (name == file || (name.size() >= 3 && file.compare(0, name.size(), name) == 0)) {
            g_pendingModelIndex = i;
            if (g_renderPath == 1 && kDemoModels[i].scene == nullptr) {
                // LumeGS renders one model at a time; follow the dropdown for
                // single-model entries (the LOD scene needs the tiling path).
                g_gsModelFile = file.c_str();
                g_gsModelDirty = true;
            }
            LOGI("setModel: %{public}s -> index %{public}d", name.c_str(), i);
            return nullptr;
        }
    }
    LOGI("setModel: unknown model '%{public}s'", name.c_str());
    return nullptr;
}

static napi_value Init(napi_env env, napi_value exports)
{
    // Save napi_env for engine initialization later.
    g_napiEnv = env;

    // Get XComponent native object.
    napi_value xcomponentObj = nullptr;
    napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcomponentObj);

    OH_NativeXComponent* nativeXComponent = nullptr;
    napi_unwrap(env, xcomponentObj, reinterpret_cast<void**>(&nativeXComponent));

    if (nativeXComponent) {
        static OH_NativeXComponent_Callback callback;
        callback.OnSurfaceCreated = OnSurfaceCreated;
        callback.OnSurfaceChanged = OnSurfaceChanged;
        callback.OnSurfaceDestroyed = OnSurfaceDestroyed;
        callback.DispatchTouchEvent = DispatchTouchEvent;
        OH_NativeXComponent_RegisterCallback(nativeXComponent, &callback);
        LOGI("XComponent callbacks registered");
    }

    // Export drawFrame so ArkTS can drive the render loop via setInterval.
    napi_property_descriptor desc[] = {
        {"drawFrame", nullptr, NapiDrawFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setModel", nullptr, NapiSetModel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setCameraMode", nullptr, NapiSetCameraMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLodBudget", nullptr, NapiSetLodBudget, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initResourceManager", nullptr, NapiInitResourceManager, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    return exports;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------
EXTERN_C_START
static napi_module s_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "lumeapp",
    .nm_priv = nullptr,
    .reserved = {0},
};
EXTERN_C_END

extern "C" __attribute__((constructor)) void RegisterLumeRenderModule(void)
{
    napi_module_register(&s_module);
}


