/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gs_loading.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <render/resource_handle.h>

#include "data/sog_parsing.h"
#include "decoder/gs_decoder.h"
#include "render/render_data_store_default_gs.h"
#include "util/gs_lod_tools.h"
#include "util/gs_parsing.h"
#include <plugin_gs/util/gs_limits.h>

#ifdef __OHOS_PLATFORM__
#ifdef __OHOS_PLATFORM_PARSING_CAMERA_POSE_ENABLED__
#include "spatial_buffer.h"
#include "spatial_codec_factory.h"
#include "spatial_meta.h"
#include "spatial_tag.h"
#endif
#endif

#include "spatial_buffer.h"
#include "spatial_codec_factory.h"
#include "spatial_meta.h"
#include "spatial_tag.h"

namespace {
#include "app/shaders/common/gs_common.h"
#include "app/shaders/common/gs_struct_common.h"
} // namespace

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS;

using namespace Spatial;

#ifdef __OHOS_PLATFORM_DISABLED__
using namespace Spatial;
#endif

static PlyFormatType ClassifyPlyFormat(const BASE_NS::vector<PlyProperty>& props, float compress)
{
    const size_t n = props.size();
    if (n == GS_FULL_HARMONICS_HEADER_SIZE) {
        return PlyFormatType::FullHarmonics;
    }
    if (n == GS_RESTRICTED_HARMONICS_HEADER_SIZE) {
        return PlyFormatType::RestrictedHarmonics;
    }
    if (n == GS_RESTRICTED_HARMONICS_HEADER_SIZE_MINIMAL) {
        // Distinguish by 4th property name:
        // Minimal:   x,y,z,f_dc_0,...  → shs before scale
        // MinimalV2: x,y,z,scale_0,... → scale before shs
        // MinimalV3: x,y,z,rot_0,...  → rotation before scale
        if (n > 3 && props[3].name.find("rot_0") != BASE_NS::string::npos) {
            return PlyFormatType::RestrictedHarmonicsMinimalV3;
        }
        if (n > 3 && props[3].name.find("scale_0") != BASE_NS::string::npos) {
            return PlyFormatType::RestrictedHarmonicsMinimalV2;
        }
        return PlyFormatType::RestrictedHarmonicsMinimal;
    }
    if (n == GS_PACKED_HEADER_SIZE && compress < 0.5f) {
        return PlyFormatType::Packed;
    }
    return PlyFormatType::Unknown;
}

static GpuBufferDesc GetStagingBufferDesc(const uint32_t byteSize)
{
    return GpuBufferDesc {
        CORE_BUFFER_USAGE_TRANSFER_SRC_BIT,
        CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT | CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS,
        byteSize,
        {},
    };
}

float GS::ComputeSplatExtent(
    const BASE_NS::Math::Vec3 covA, const BASE_NS::Math::Vec3 covB, const float stdDevMultiplier)
{
    const float covData[9u] = { covA.x, covA.y, covA.z, covA.y, covB.x, covB.y, covA.z, covB.y, covB.z };
    const BASE_NS::Math::Mat3X3 covMat(covData);

    constexpr size_t POWER_ITERATIONS = 2u;
    BASE_NS::Math::Vec3 vv = BASE_NS::Math::Vec3(1.0f, 0.0f, 0.0f);
    for (size_t ii = 0; ii < POWER_ITERATIONS; ii++) {
        vv = BASE_NS::Math::Normalize(covMat * vv);
    }

    const float maxEigenvalue = BASE_NS::Math::Dot(vv, covMat * vv);
    return BASE_NS::Math::sqrt(maxEigenvalue) * stdDevMultiplier;
}

void CreateGaussianRenderTargets(GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx, GpuImageDesc& desc,
    const BASE_NS::Math::UVec2 renderSize, const BASE_NS::Math::UVec2 resolution)
{
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();
    const float upscaleFactor = loadCtx.dataStoreGs_->GetXegUpscaleFactor();
    desc.width = renderSize.x;
    desc.height = renderSize.y;
    renderData->gaussianRenderTarget0 = gpuResourceMgr.Create("GS3DRenderTarget0_" + loadCtx.uniqueName, desc);

    // gaussianRenderTarget1: high resolution for XEG upscale output
    // Use original output resolution directly to avoid precision loss
    if (upscaleFactor > 1.001f) {
        desc.width = resolution.x;
        desc.height = resolution.y;
        renderData->gaussianRenderTarget1 = gpuResourceMgr.Create("GS3DRenderTarget1_" + loadCtx.uniqueName, desc);
        desc.width = 2u;
        desc.height = 2u;
        // gaussianRenderTarget2/3: lowest resolution, currently unused
        renderData->gaussianRenderTarget2 = gpuResourceMgr.Create("GS3DRenderTarget2_" + loadCtx.uniqueName, desc);
        renderData->gaussianRenderTarget3 = gpuResourceMgr.Create("GS3DRenderTarget3_" + loadCtx.uniqueName, desc);
    } else {
        desc.width = renderSize.x;
        desc.height = renderSize.y;
        renderData->gaussianRenderTarget1 = gpuResourceMgr.Create("GS3DRenderTarget1_" + loadCtx.uniqueName, desc);
        renderData->gaussianRenderTarget2 = gpuResourceMgr.Create("GS3DRenderTarget2_" + loadCtx.uniqueName, desc);
        renderData->gaussianRenderTarget3 = gpuResourceMgr.Create("GS3DRenderTarget3_" + loadCtx.uniqueName, desc);
    }

    desc.width = renderSize.x;
    desc.height = renderSize.y;
    if (!renderData->gsEnableDepthPass) {
        desc.width = 2u;
        desc.height = 2u;
    }
    renderData->gaussianRenderTargetBB = gpuResourceMgr.Create("GS3DRenderTargetBB_" + loadCtx.uniqueName, desc);

    desc.format = Format::BASE_FORMAT_R16G16_SFLOAT;
    renderData->gaussianRenderTargetDepth1 =
        gpuResourceMgr.Create("GS3DRenderTargetDepth1_" + loadCtx.uniqueName, desc);
}

void CreateGsRenderImages(
    GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx, const BASE_NS::Math::UVec2 resolution)
{
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();
    const uint32_t depthSize = renderData->gsEnableDepthPass ? GS_DATA_STORE_TEX_SIZE : 2u;

    GpuImageDesc desc;
    desc.depth = 1;
    desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    desc.usageFlags =
        ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_STORAGE_BIT;
    desc.imageType = ImageType::CORE_IMAGE_TYPE_2D;
    desc.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
    desc.imageViewType = ImageViewType::CORE_IMAGE_VIEW_TYPE_2D;
    desc.engineCreationFlags = EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
    desc.format = Format::BASE_FORMAT_R32_SFLOAT;
    desc.width = depthSize;
    desc.height = depthSize;
    renderData->depthStoreCacheHandle = gpuResourceMgr.Create("GS3DDepthStorageCache_" + loadCtx.uniqueName, desc);
    desc.format =
        renderData->use16bitSurfaceTarget ? Format::BASE_FORMAT_R16G16B16A16_UNORM : Format::BASE_FORMAT_R8G8B8A8_UNORM;
    desc.usageFlags =
        ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        ImageUsageFlagBits::CORE_IMAGE_USAGE_STORAGE_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT;

    const float scaleFactor = static_cast<float>(loadCtx.dataStoreGs_->GetGsScaleFactor());
    const float upscaleFactor = loadCtx.dataStoreGs_->GetXegUpscaleFactor();
    const BASE_NS::Math::UVec2 renderSize(static_cast<uint32_t>(ceil(resolution.x / scaleFactor / upscaleFactor)),
        static_cast<uint32_t>(ceil(resolution.y / scaleFactor / upscaleFactor)));

    CreateGaussianRenderTargets(renderData, loadCtx, desc, renderSize, resolution);
}

void ClearGsRenderTargets(GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx)
{
    const RENDER_NS::ClearColorValue clearValue(0.0f, 0.0f, 0.0f, 0.0f);
    loadCtx.dsStaging_->ClearColorImage(renderData->gaussianRenderTarget0, clearValue);
    loadCtx.dsStaging_->ClearColorImage(renderData->gaussianRenderTarget1, clearValue);
    loadCtx.dsStaging_->ClearColorImage(renderData->gaussianRenderTarget2, clearValue);
    loadCtx.dsStaging_->ClearColorImage(renderData->gaussianRenderTarget3, clearValue);
    loadCtx.dsStaging_->ClearColorImage(renderData->gaussianRenderTargetBB, clearValue);
}

void CreateGsStatusUbo(GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx)
{
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();

    GsRenderStatusUbo renderStatusUbo;
    renderStatusUbo.jitterFrameRendered = 0;
    renderStatusUbo.frameIdx = 0;
    renderStatusUbo.renderScaleFactor = loadCtx.dataStoreGs_->GetRenderScaleFactor();
    renderStatusUbo.DRSScale = loadCtx.dataStoreGs_->GetDRSScale();

    renderStatusUbo.sharpeningCoeff =
        static_cast<const GS::RenderDataStoreDefaultGS*>(loadCtx.dataStoreGs_.get())->GetRenderSharpeningCoefficient();

    renderData->gaussianRenderTargetStatusUbo =
        CreateGSBuffer(gpuResourceMgr, "GS3DRenderStatusUbo_" + loadCtx.uniqueName, sizeof(GsRenderStatusUbo),
            BASE_NS::array_view<uint8_t>((uint8_t*)&renderStatusUbo, sizeof(GsRenderStatusUbo)),
            BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT,
            MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

void PushGsMaterialResources(GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx,
    CORE3D_NS::IRenderHandleComponentManager& rhcm, CORE3D_NS::MaterialComponent& materialComponent)
{
    if (!renderData) {
        return;
    }
    if (!loadCtx.ecs_ || !loadCtx.renderContext_) {
        return;
    }
    auto& em = loadCtx.ecs_->GetEntityManager();
    auto& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();

    auto samplerNearestRhr = gpuResourceMgr.GetSamplerHandle("CORE_DEFAULT_SAMPLER_NEAREST_REPEAT");
    if (!samplerNearestRhr) {
        CORE_LOG_E("Failed to get sampler handle");
        return;
    }
    renderData->sampler = samplerNearestRhr;
    auto samplerLinearRhr = gpuResourceMgr.GetSamplerHandle("CORE_DEFAULT_SAMPLER_LINEAR_CLAMP");

    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTarget0));
    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTarget1));
    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTarget2)); 
    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTarget3));
    materialComponent.customResources.push_back(GetOrCreateEntityReference(em, rhcm, samplerNearestRhr));
    materialComponent.customResources.push_back(GetOrCreateEntityReference(em, rhcm, samplerLinearRhr));
    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTargetStatusUbo));
    materialComponent.customResources.push_back(
        GetOrCreateEntityReference(em, rhcm, renderData->gaussianRenderTargetDepth1));
}

void SetupGsMaterial(GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx,
    const BASE_NS::Math::UVec2 resolution, CORE3D_NS::IRenderHandleComponentManager& rhcm)
{
    if (!loadCtx.ecs_ || !loadCtx.renderContext_) {
        return;
    }
    auto* materialManager = GetManager<CORE3D_NS::IMaterialComponentManager>(*loadCtx.ecs_);
    if (!materialManager) {
        CORE_LOG_E("SetupGsMaterial: failed to get IMaterialComponentManager!");
        return;
    }
    auto& em = loadCtx.ecs_->GetEntityManager();
    auto& device = loadCtx.renderContext_->GetDevice();

    renderData->gsMaterial = em.CreateReferenceCounted();
    materialManager->Create(renderData->gsMaterial);
    auto matHandle = materialManager->Write(renderData->gsMaterial);
    if (!matHandle) {
        materialManager->Destroy(renderData->gsMaterial);
        return;
    }

    CORE3D_NS::MaterialComponent& materialComponent = *matHandle;
    auto& shaderMgr = device.GetShaderManager();
    auto shaderHandle = shaderMgr.GetShaderHandle("gsshaders://shader/gs3d_gaussian_splatting.shader");
    if (!RenderHandleUtil::IsValid(shaderHandle.GetHandle())) {
        CORE_LOG_E("Failed to get shader handle");
        return;
    }
    materialComponent.materialShader.shader = GetOrCreateEntityReference(em, rhcm, shaderHandle);
    auto defaultGfxStateHandle = shaderMgr.GetGraphicsStateHandleByShaderHandle(shaderHandle);
    if (defaultGfxStateHandle) {
        auto graphicsState = shaderMgr.GetGraphicsState(defaultGfxStateHandle);
        graphicsState.depthStencilState.enableDepthWrite = renderData->gsEnableDepthPass;
        RENDER_NS::IShaderManager::GraphicsStateCreateInfo createInfo{};
        createInfo.path = "gs3d_custom_depth_state";
        createInfo.graphicsState = graphicsState;

        RENDER_NS::IShaderManager::GraphicsStateVariantCreateInfo variantInfo{};
        variantInfo.renderSlot = "CORE3D_RS_DM_FW_TRANSLUCENT";
        variantInfo.variant = "TRANSLUCENT_FW";

        auto customGfxHandle = shaderMgr.CreateGraphicsState(createInfo, variantInfo);

        if (customGfxHandle) {
            materialComponent.materialShader.graphicsState = GetOrCreateEntityReference(em, rhcm, customGfxHandle);
        }
    }
    materialComponent.type = MaterialComponent::Type::CUSTOM_COMPLEX;
    materialComponent.materialLightingFlags = 0u;

    CreateGsRenderImages(renderData, loadCtx, resolution);
    ClearGsRenderTargets(renderData, loadCtx);
    CreateGsStatusUbo(renderData, loadCtx);
    PushGsMaterialResources(renderData, loadCtx, rhcm, materialComponent);
}

void SetupGsMesh(
    GS::GsRenderData* renderData, const GS::GsLoadContext& loadCtx, CORE3D_NS::RenderMeshComponent& renderMeshComponent)
{
    auto* mcm = GetManager<CORE3D_NS::IMeshComponentManager>(*loadCtx.ecs_);
    auto& em = loadCtx.ecs_->GetEntityManager();

    renderData->gsMesh = em.CreateReferenceCounted();
    renderMeshComponent.mesh = renderData->gsMesh;
    mcm->Create(renderMeshComponent.mesh);
    auto meshHandle = mcm->Write(renderMeshComponent.mesh);
    if (!meshHandle) {
        return;
    }

    CORE3D_NS::MeshComponent& meshComponent = *meshHandle;
    meshComponent.submeshes.resize(1u);
    meshComponent.submeshes[0].material = renderData->gsMaterial;
    meshComponent.submeshes[0].vertexCount = 4u;
    meshComponent.submeshes[0].indexCount = 0;
    meshComponent.submeshes[0].instanceCount = 1u;
    meshComponent.submeshes[0].aabbMin = Math::Vec3(std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    meshComponent.submeshes[0].aabbMax = Math::Vec3(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
}

void GS::CreateGsRenderTargets(GS::GsRenderData* renderData, CORE_NS::Entity targetEntity,
    const GS::GsLoadContext loadCtx, const BASE_NS::Math::UVec2 resolution, const bool resolutionRecreate)
{
    if (!renderData) {
        return;
    }
    if (!loadCtx.renderContext_ || !loadCtx.ecs_ || !loadCtx.renderMeshManager_ || !loadCtx.dataStoreGs_) {
        return;
    }

    if (!EntityUtil::IsValid(targetEntity)) {
        CORE_LOG_E("CreateGsRenderTargets: targetEntity not valid!");
        return;
    }

    CORE_LOG_I("CreateGsRenderTargets: recreate render targets.");

    auto* rhcm = GetManager<CORE3D_NS::IRenderHandleComponentManager>(*loadCtx.ecs_);
    if (!rhcm) {
        CORE_LOG_E("GS::CreateGsRenderTargets: failed to get IRenderHandleComponentManager!");
        return;
    }

    if (renderData->isMaterialValid && !resolutionRecreate) {
        return;
    }

    renderData->isMaterialValid = true;

    loadCtx.renderMeshManager_->Create(targetEntity);
    auto renderMeshHandle = loadCtx.renderMeshManager_->Write(targetEntity);
    if (!renderMeshHandle) {
        loadCtx.renderMeshManager_->Destroy(targetEntity);
        return;
    }

    SetupGsMaterial(renderData, loadCtx, resolution, *rhcm);
    SetupGsMesh(renderData, loadCtx, *renderMeshHandle);
}

void GS::RemoveGsRenderTargets(
    GS::GsRenderData* renderData, CORE_NS::Entity targetEntity, const GS::GsLoadContext loadCtx)
{
    if (!loadCtx.renderContext_ || !loadCtx.ecs_ || !loadCtx.renderMeshManager_) {
        return;
    }

    if (!EntityUtil::IsValid(targetEntity)) {
        CORE_LOG_E("RemoveGsRenderTargets: targetEntity not valid!");
    }

    CORE_LOG_I("RemoveGsRenderTargets: remove render targets.");

    IEcs& ecs_ = *loadCtx.ecs_;
    CORE3D_NS::IMeshComponentManager* mcm = GetManager<CORE3D_NS::IMeshComponentManager>(ecs_);

    mcm->Destroy(targetEntity);
}

GS::GsPrimitive GS::LoadGaussianData(const BASE_NS::string& gsPath, GS::GsRenderData* renderData, GsComponent& dfc,
    Entity splatEntity, const GS::GsLoadContext loadCtx)
{
    if (!loadCtx.renderContext_ || !loadCtx.dataStoreGs_ || !loadCtx.threadPool_ || !loadCtx.dsStaging_ ||
        !renderData || !loadCtx.shuttingDown_) {
        return {};
    }

    GsPrimitive primitive;
    GsObjectLoadDataExt loadData;

    CORE_NS::IFileManager& fileManager = loadCtx.renderContext_->GetEngine().GetFileManager();
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();

    if (!CheckValidFileExtensions(gsPath)) {
        CORE_LOG_E(
            "Loading gaussians failed: invalid file extension, only .ply, .glb, .splat, and .mp4 (read with offset) "
            "are supported.");
        primitive.isValid = false;
        return primitive;
    }

    CORE_CPU_PERF_BEGIN(readPly, "LUMEGS", "ReadPLYFile", "", 0xFFFF00);
    IFile::Ptr gsFile = fileManager.OpenFile(gsPath);

    if (!gsFile) {
        CORE_LOG_E("Loading gaussians failed: could not open GS file at %s!", gsPath.c_str());
        primitive.isValid = false;
        CORE_CPU_PERF_END(readPly);
        return primitive;
    }

    size_t fileTotalByteLen = static_cast<size_t>(gsFile->GetLength());
    size_t fileByteLen = fileTotalByteLen;

    if (IsSogExtension(gsPath) && fileByteLen > GS::MAX_SOG_FILE_SIZE) {
        CORE_LOG_E("SOG file too large: %zu bytes, limit %zu", fileByteLen, GS::MAX_SOG_FILE_SIZE);
        primitive.isValid = false;
        CORE_CPU_PERF_END(readPly);
        return primitive;
    }

    if (dfc.gsDataOffset > 0) {
        if (dfc.gsDataOffset >= static_cast<int64_t>(fileTotalByteLen)) {
            CORE_LOG_E("Loading gaussians failed: offset exceeds data length!");
            primitive.isValid = false;
            CORE_CPU_PERF_END(readPly);
            return primitive;
        }
        if (!gsFile->Seek(dfc.gsDataOffset)) {
            CORE_LOG_E("Seek failed");
            return primitive;
        }
        fileByteLen = fileTotalByteLen - static_cast<size_t>(dfc.gsDataOffset);
    }

    ByteArray fileDataArray(fileByteLen);
    auto bytesRead = gsFile->Read(fileDataArray.GetData().data(), fileByteLen);
    if (bytesRead != fileByteLen) {
        CORE_LOG_E("Partial read");
        return primitive;
    }
    gsFile->Close();
    CORE_CPU_PERF_END(readPly);

    if (loadCtx.shuttingDown_->load()) { // check exit flag
        CORE_LOG_I("GS async load cancelled.");
        return {};
    }

    PlyHeader gsHeader;
    GS_DECODER::GsBasicInfo gsBasicInfo;
    GS_DECODER::UwaGsHeader header;
    uint32_t numGaussians = 0u;
    bool isCompressedGs = false;
    const bool isSog = IsSogExtension(gsPath);
    std::shared_ptr<const std::vector<uint8_t>> encodedData;
    std::vector<std::shared_ptr<const SpatialMeta>> primitiveMetas;
    auto demuxer = SpatialCodecFactory::CreateDemuxer(SpatialTag::UWAGS);
    std::string decVersion;
    std::shared_ptr<SpatialModel> model;
    std::shared_ptr<const SpatialMeta> basicInfo;

    const bool isValidGltf = !isSog && CheckGltfAndResetSize(fileDataArray.GetData().data(), fileByteLen);
    if (fileByteLen > fileDataArray.GetData().size()) {
        CORE_LOG_E("The GLB file with offset reported length exceeds data!");
        primitive.isValid = false;
        return primitive;
    }

#ifdef __OHOS_PLATFORM_PARSING_CAMERA_POSE_ENABLED__ // explicitly disabled due to loading errors
    {
        bool isGlbFile = gsPath.ends_with(".glb") || gsPath.ends_with(".GLB") ||
                         gsPath.ends_with(".mp4") || gsPath.ends_with(".MP4");
        bool enableEdit = loadCtx.gsConfigComponent_ ? loadCtx.gsConfigComponent_->enableGsEditor : false;
        if (!isGlbFile || !enableEdit) {
            CORE_LOG_I("Not a .glb file or not enableEdit, skip camera motion parsing.");
        } else {
            const uint8_t* rawData = fileDataArray.GetData().data();
            size_t rawSize = fileDataArray.GetData().size();
            if (!rawData || rawSize == 0) {
                CORE_LOG_E("Error: fileDataArray is empty");
                return {};
            }

            std::vector<uint8_t> fileData(rawData, rawData + rawSize);
            auto inputBuffer = SpatialCodecFactory::CreateBuffer(SpatialTag::UWAGS);
            if (!inputBuffer) {
                CORE_LOG_D("Error: Failed to create SpatialBuffer");
                return {};
            }
            auto ret = inputBuffer->SetBinaryData(std::move(fileData));
            if (ret != SpatialErrorCode::SUCCESS) {
                CORE_LOG_D("Error: Failed to set data to SpatialBuffer");
                return {};
            }

            auto demuxer = SpatialCodecFactory::CreateDemuxer(SpatialTag::UWAGS);
            if (!demuxer) {
                CORE_LOG_D("Error: Failed to create GLB UWA 3DGS Demuxer");
                return {};
            }
            auto config = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
            if (!config) {
                CORE_LOG_D("Error: Failed to create SpatialMeta config");
                return {};
            }
            ret = demuxer->Initialize(inputBuffer, *config);
            if (ret != SpatialErrorCode::SUCCESS) {
                CORE_LOG_D("Error: Failed to initialize demuxer, code: %d", static_cast<int>(ret));
                return {};
            }
            CORE_LOG_D("Demuxer initialized successfully.");

            size_t bufferIndex = 0;
            std::string inputname = std::to_string(bufferIndex);
            std::shared_ptr<const SpatialBuffer> buffer;
            auto inputDesc = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
            if (!inputDesc) {
                CORE_LOG_D("Error: Failed to create SpatialMeta inputDesc");
                return {};
            }
            ret = demuxer->GetData(inputname, *inputDesc, buffer);

            std::vector<std::shared_ptr<const SpatialMeta>> cameraMetas;
            ret = demuxer->GetMeta(SpatialTag::CAMERA_MOTION_PARAMS, cameraMetas);

            if (ret == SpatialErrorCode::SUCCESS && !cameraMetas.empty()) {
                CORE_LOG_D("Found %zu camera motion metadata entries.", cameraMetas.size());

                size_t totalFrames = 0;
                for (const auto& meta : cameraMetas) {
                    auto motionMeta = std::const_pointer_cast<SpatialMeta>(meta);
                    auto timestamps = motionMeta->GetMetaAs<std::vector<float>>(SpatialTag::TIMESTAMPS);
                    if (timestamps.has_value()) {
                        totalFrames += timestamps.value().size();
                    }
                }

                dfc.cameraMotion2d.resize(totalFrames * 8);

                size_t frameWriteIdx = 0;
                for (const auto& meta : cameraMetas) {
                    auto motionMeta = std::const_pointer_cast<SpatialMeta>(meta);

                    auto timestamps = motionMeta->GetMetaAs<std::vector<float>>(SpatialTag::TIMESTAMPS);
                    auto translations =
                        motionMeta->GetMetaAs<std::vector<float>>(SpatialTag::TRANSLATION);
                    auto rotations = motionMeta->GetMetaAs<std::vector<float>>(SpatialTag::ROTATION);
                    auto yfovs = motionMeta->GetMetaAs<std::vector<float>>(SpatialTag::YFOV);

                    if (!timestamps.has_value() || !translations.has_value() || !rotations.has_value() ||
                        !yfovs.has_value()) {
                        continue;
                    }

                    size_t numFrames = timestamps.value().size();

                    if (translations.value().size() < numFrames * 3 || rotations.value().size() < numFrames * 4 ||
                        yfovs.value().size() < numFrames) {
                        continue;
                    }

                    for (size_t i = 0; i < numFrames; ++i, ++frameWriteIdx) {
                        size_t base = frameWriteIdx * 8;

                        dfc.cameraMotion2d[base + 0] = translations.value()[i * 3 + 0];
                        dfc.cameraMotion2d[base + 1] = translations.value()[i * 3 + 1];
                        dfc.cameraMotion2d[base + 2] = translations.value()[i * 3 + 2];

                        dfc.cameraMotion2d[base + 3] = rotations.value()[i * 4 + 0];
                        dfc.cameraMotion2d[base + 4] = rotations.value()[i * 4 + 1];
                        dfc.cameraMotion2d[base + 5] = rotations.value()[i * 4 + 2];
                        dfc.cameraMotion2d[base + 6] = rotations.value()[i * 4 + 3];

                        dfc.cameraMotion2d[base + 7] = yfovs.value()[i];
                    }
                }

                CORE_LOG_D("Total motion frames stored: %zu", totalFrames);
            } else {
                CORE_LOG_D("No camera motion metadata found.");
            }
        }
    }
#endif

    if (isValidGltf) {
        CORE_LOG_I("glTF file found, unpacking.");

        auto validExtension = GetGltfExtensions(fileDataArray.GetData().data(), fileByteLen);
        if (!validExtension.first) {
            CORE_LOG_E("Failed to get glTF Extensions!");
            primitive.isValid = false;
            return primitive;
        }

        if (validExtension.second == "UWA_primitive_3DGS_compression") {
            isCompressedGs = true;
            ByteArray streamData = ExtractGltfCompressedData(fileDataArray.GetData().data(), fileByteLen);
            if (streamData.GetData().empty()) {
                CORE_LOG_E("Failed to extract glTF stream data!");
                primitive.isValid = false;
                return primitive;
            }

            fileDataArray = BASE_NS::move(streamData);
            fileByteLen = fileDataArray.GetData().size();
        } else if (validExtension.second == "UWA_gaussian_splatting") {
            ByteArray plyData = ExtractGltfPlyData(fileDataArray.GetData().data(), fileByteLen);
            if (plyData.GetData().empty()) {
                CORE_LOG_E("Failed to extract glTF ply data!");
                primitive.isValid = false;
                return primitive;
            }

            fileDataArray = BASE_NS::move(plyData);
            fileByteLen = fileDataArray.GetData().size();
        } else {
            if (!SpatialCodecFactory::IsLoaded()) {
                CORE_LOG_E("SpatialCodec library not loaded, cannot parse compressed GLB");
                primitive.isValid = false;
                return primitive;
            }
            const uint8_t* rawData = fileDataArray.GetData().data();
            std::vector<uint8_t> fileData(rawData, rawData + fileByteLen);
            auto inputBuffer = SpatialCodecFactory::CreateBuffer(SpatialTag::UWAGS);
            if (!inputBuffer) {
                CORE_LOG_E("Failed to create SpatialBuffer");
                primitive.isValid = false;
                return primitive;
            }
            inputBuffer->SetBinaryData(std::move(fileData));

            // initialize demuxer
            demuxer = SpatialCodecFactory::CreateDemuxer(SpatialTag::UWAGS);
            if (!demuxer) {
                CORE_LOG_E("Failed to create SpatialCodecDemuxer");
                primitive.isValid = false;
                return primitive;
            }
            auto config = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
            if (!config) {
                CORE_LOG_E("Failed to create SpatialMeta config");
                primitive.isValid = false;
                return primitive;
            }
            auto ret = demuxer->Initialize(inputBuffer, *config);
            if (ret != SpatialErrorCode::SUCCESS) {
                CORE_LOG_E("Failed to initialize Spatial Codec, error code: %d", static_cast<int>(ret));
                primitive.isValid = false;
                return primitive;
            }

            std::shared_ptr<const SpatialBuffer> buffer;
            auto dataDesc = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
            if (!dataDesc) {
                CORE_LOG_E("Failed to create SpatialMeta dataDesc");
                primitive.isValid = false;
                return primitive;
            }
            ret = demuxer->GetData("0", *dataDesc, buffer);
            if (ret != SpatialErrorCode::SUCCESS) {
                CORE_LOG_E("Failed to extract glTF stream data!");
                primitive.isValid = false;
                return primitive;
            }


            // get glb meta information
            ret = demuxer->GetMeta(SpatialTag::BASIC_INFO, primitiveMetas);
            if (ret != SpatialErrorCode::SUCCESS) {
                CORE_LOG_E("Failed to get metadata, error code: %d", static_cast<int>(ret));
                primitive.isValid = false;
                return primitive;
            }
            // get codec version through extension name
            if (!primitiveMetas.empty()) {
                numGaussians = primitiveMetas[0]->GetMetaAs<uint32_t>(SpatialTag::GS_POINTS_NUM).value_or(0);
                primitive.shDegree = primitiveMetas[0]->GetMetaAs<uint32_t>(SpatialTag::SH_DEGREE).value_or(0);
                decVersion = std::get<std::string>(primitiveMetas[0]->
                    GetMeta(SpatialTag::COMPRESSION_EXTENSION_NAME).value_or("None"));
            } else {
                CORE_LOG_E("Get metadata failed");
                primitive.isValid = false;
                return primitive;
            }
            ret = buffer->GetBinaryData(encodedData);
            if (ret != SpatialErrorCode::SUCCESS || !encodedData) {
                CORE_LOG_E("Error: Failed to get binary data from buffer");
                primitive.isValid = false;
                return primitive;
            }

            bool success;
            if (decVersion == "UWA_primitive_3DGS_compression" ||
                decVersion == "UWA_gaussian_splatting_compression_EGSC") {
                isCompressedGs = true;
                success = ParseCompressedBasicInfo(*encodedData, decVersion, primitive, model, basicInfo);
            } else if (validExtension.second == "KHR_gaussian_splatting") {
                isCompressedGs = false;
            } else {
                CORE_LOG_E("Current extention is Not support: %s", decVersion.c_str());
                primitive.isValid = false;
                return primitive;
            }

            if (isCompressedGs && success) {
                numGaussians = static_cast<size_t>((basicInfo->
                    GetMetaAs<uint32_t>(SpatialTag::GS_POINTS_NUM)).value_or(0));
            } else if (isCompressedGs && !success) {
                CORE_LOG_E("Failed to parse basic info for compressed file");
                primitive.isValid = false;
                return primitive;
            }
        }
    }

    size_t headerEndIdx = 0;

    if (isCompressedGs) {
        if (decVersion == "UWA_gaussian_splatting_compression_EGSC") {
        } else {
            bool succeed = GS_DECODER::ParseBasicInfo(fileDataArray.GetData().data(), fileByteLen, gsBasicInfo, header);
            if (!succeed) {
                CORE_LOG_E("Failed to parse GsBasicInfo!");
                primitive.isValid = false;
                return primitive;
            }
            numGaussians = static_cast<uint32_t>(gsBasicInfo.numGS);
        }
    } else if (encodedData) {
    } else if (!isSog) {
        gsHeader = LoadPLYHeaderCompressed(fileDataArray.GetData().data(), fileByteLen, headerEndIdx);
        // if LoadPLYHeaderCompressed fails, try LoadPLYHeader
        if (gsHeader.vertexProperties.size() == 0) {
            gsHeader = LoadPLYHeader(fileDataArray.GetData().data(), fileByteLen, headerEndIdx);
        }
        if (gsHeader.numVertices == 0) {
            CORE_LOG_E("Invalid GS3D PLY header!");
            primitive.isValid = false;
            return primitive;
        }
        numGaussians = static_cast<uint32_t>(gsHeader.numVertices);
    }

    if (loadCtx.shuttingDown_->load()) { // check exit flag
        CORE_LOG_I("GS async load cancelled.");
        return {};
    }

    if (numGaussians > 0u) {
        if (numGaussians > MAX_GAUSSIAN_COUNT) {
            CORE_LOG_E("numGaussians exceeds maximum allowed limit");
            primitive.isValid = false;
            return primitive;
        }
        primitive.numGs = numGaussians;
    }
    // override with config if exists
    renderData->convertGsColorSrgb =
        loadCtx.gsConfigComponent_ ? loadCtx.gsConfigComponent_->convertGsColorSrgb : dfc.convertGsColorSrgb;

    const PlyFormatType formatType =
        (!isCompressedGs && !isSog) ? ClassifyPlyFormat(gsHeader.vertexProperties, gsHeader.compress)
                                    : PlyFormatType::Unknown;

    if (isCompressedGs) {
        CORE_LOG_I("isCompressedGs");
        primitive.codecVersion = 1; // currently only one compression version so assign it directly
        // current skip 0.4
        if (decVersion == "UWA_gaussian_splatting_compression_EGSC") {
            primitive.codecVersion = 2;
        }
    } else if (encodedData) {
    } else if (isSog) {
        // Header validation deferred to ParseGSSog.
    } else {
        // header validation
        switch (formatType) {
            case PlyFormatType::FullHarmonics:
                if (fileByteLen - headerEndIdx !=
                    sizeof(PlyVertexDataFullHarmonics) * static_cast<size_t>(numGaussians)) {
                    CORE_LOG_E("Invalid GS3D header size for full harmonics!");
                    primitive.isValid = false;
                    return primitive;
                }
                break;
            case PlyFormatType::RestrictedHarmonics:
                if (fileByteLen - headerEndIdx !=
                    sizeof(PlyVertexDataRestrictedHarmonics) * static_cast<size_t>(numGaussians)) {
                    CORE_LOG_E("Invalid GS3D header size for restricted harmonics!");
                    primitive.isValid = false;
                    return primitive;
                }
                break;
            case PlyFormatType::RestrictedHarmonicsMinimal:
                if (fileByteLen - headerEndIdx !=
                    sizeof(PlyVertexDataRestrictedHarmonicsMinimal) * static_cast<size_t>(numGaussians)) {
                    CORE_LOG_E("Invalid GS3D header size for minimal restricted harmonics!");
                    primitive.isValid = false;
                    return primitive;
                }
                break;
            case PlyFormatType::RestrictedHarmonicsMinimalV2:
                if (fileByteLen - headerEndIdx !=
                    sizeof(PlyVertexDataRestrictedHarmonicsMinimalV2) * static_cast<size_t>(numGaussians)) {
                    CORE_LOG_E("Invalid GS3D header size for minimal V2 restricted harmonics!");
                    primitive.isValid = false;
                    return primitive;
                }
                break;
            case PlyFormatType::RestrictedHarmonicsMinimalV3:
                if (fileByteLen - headerEndIdx !=
                    sizeof(PlyVertexDataRestrictedHarmonicsMinimalV3) * static_cast<size_t>(numGaussians)) {
                    CORE_LOG_E("Invalid GS3D header size for minimal V3 restricted harmonics!");
                    primitive.isValid = false;
                    return primitive;
                }
                break;
            case PlyFormatType::Packed:
                break; // compress already validated in ClassifyPlyFormat
            default:
                CORE_LOG_E("Invalid gaussian header or not supported!");
                primitive.isValid = false;
                return primitive;
        }

        // shDegree
        switch (formatType) {
            case PlyFormatType::FullHarmonics:
                primitive.shDegree = 3u;
                break;
            case PlyFormatType::RestrictedHarmonics:
            case PlyFormatType::RestrictedHarmonicsMinimal:
            case PlyFormatType::RestrictedHarmonicsMinimalV2:
            case PlyFormatType::RestrictedHarmonicsMinimalV3:
                primitive.shDegree = 0;
                break;
            case PlyFormatType::Packed:
                primitive.shDegree = static_cast<uint32_t>(gsHeader.shDegree);
                break;
            default:
                break;
        }
    }
    primitive.shDegree = Math::min(primitive.shDegree, dfc.maxShDegreeOverride);

    // For SOG files, allocations are handled inside ParseGSSog.
    if (!isSog) {
        // only when need sh buffer, we alloc the mem
        if (primitive.RequireSH()) {
            loadData.sphericalHarmonics.resize(numGaussians);
        }

        loadData.gsVerticesData.Resize(numGaussians);
        loadData.gaussianPositions.resize(numGaussians);
        loadData.gaussianIndices.resize(numGaussians);
        if (renderData->enableScoreCulling) {
            loadData.gaussianMaxScales.resize(numGaussians);
        }
    }

    if (renderData->CanUsePackedData()) {
        // pack it to save memory
        loadData.gsVerticesData.SetPackingEnabled(true);
    }
    loadData.gsVerticesData.SetGPUAllocator(gpuResourceMgr);

    loadData.gsVerticesData.Resize(numGaussians);
    loadData.gaussianPositions.resize(numGaussians);
    loadData.gaussianIndices.resize(numGaussians);
    if (renderData->enableScoreCulling) {
        loadData.gaussianMaxScales.resize(numGaussians);
    }

    // multi-threaded gaussian loading
    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "ParseGSData", "", 0xFF00FF);

        // set empty ASTC to avoid error
        if (renderData->gsShASTCDataRefs.size() == 0) {
            SetDefaultASTC(renderData, gpuResourceMgr, loadCtx);
        }

        // decode attributes to CPU
        if (isCompressedGs) {
            // get texidx
            bool seen[GS_MAX_NUM_GS_TEXTURES] = { false };
            size_t primitiveIndexs[GS_MAX_NUM_GS_TEXTURES] = { 0u };
            const size_t primitivesNum = loadCtx.dataStoreGs_->GetGsPrimitives().size();

            for (size_t ii = 0; ii < primitivesNum; ii++) {
                const size_t textIdOffset =
                    static_cast<size_t>(loadCtx.dataStoreGs_->GetGsPrimitives()[ii].textIdOffset);
                const size_t attribute2DTexNum =
                    static_cast<size_t>(loadCtx.dataStoreGs_->GetGsPrimitives()[ii].attribute2DTexNum);
                for (size_t jj = 0; jj < attribute2DTexNum; jj++) {
                    const size_t texSlot = textIdOffset + jj;
                    if (texSlot >= GS_MAX_NUM_GS_TEXTURES) {
                        CORE_LOG_E("GS texture slot out of range during slot accounting; skipping primitive %zu.", ii);
                        break;
                    }
                    seen[texSlot] = true;
                    primitiveIndexs[texSlot] = ii;
                }
            }

            // ASTC texture slot defragmentation
            for (size_t ii = 0; ii < GS_MAX_NUM_GS_TEXTURES; ii++) {
                if (!seen[ii]) {
                    size_t count = 0;
                    for (size_t jj = ii + 1; jj < GS_MAX_NUM_GS_TEXTURES; jj++) {
                        if (seen[jj]) {
                            if (ii + count >= GS_MAX_NUM_GS_TEXTURES) {
                                break;
                            }

                            renderData->gsShASTCDataRefs[ii + count] = renderData->gsShASTCDataRefs[jj];
                            renderData->astcSizes[ii + count] = renderData->astcSizes[jj];
                            seen[ii + count] = true;
                            seen[jj] = false;
                            if (count == 0) {
                                loadCtx.dataStoreGs_->GetGsPrimitives()[primitiveIndexs[jj]].textIdOffset =
                                    static_cast<uint32_t>(ii);
                            }
                            count += 1;
                        } else if (count > 0) {
                            break;
                        }
                    }
                    if (count == 0) {
                        primitive.textIdOffset = static_cast<uint32_t>(ii);
                        break;
                    }
                    ii += count - 1;
                }
            }
            bool succeed;
            if (decVersion == "UWA_gaussian_splatting_compression_EGSC") {
                succeed = ParseGSCompressed(model, basicInfo, decVersion, renderData, &loadData,
                    primitive, loadCtx.threadPool_.get(), gpuResourceMgr, loadCtx.renderContext_, loadCtx);
            } else {
                succeed = ParseGSCompressed(fileDataArray.GetData().data(), fileByteLen, renderData, &loadData,
                    primitive, loadCtx.threadPool_.get(), gpuResourceMgr, loadCtx.renderContext_, loadCtx);
            }
            if (loadCtx.shuttingDown_->load()) { // check exit flag
                CORE_LOG_I("GS async load cancelled.");
                return {};
            }
            if (!succeed) {
                CORE_LOG_E("Decoding compressed file failed!");
                primitive.isValid = false;
                return primitive;
            }
        } else if (encodedData) {
            // parse gltf containing uncompressed 3dgs
            bool succeed = ParseGSUnCompressed(encodedData,
                primitiveMetas, numGaussians, renderData, &loadData, primitive, loadCtx);
            if (!succeed) {
                CORE_LOG_E("parsing uncompressed file failed!");
                primitive.isValid = false;
                return primitive;
            } else {
                CORE_LOG_E("parsing uncompressed gltf done");
            }
        } else if (isSog) {
            CORE_NS::IImageLoaderManager& imgMgr = loadCtx.renderContext_->GetEngine().GetImageLoaderManager();

            GsObjectLoadData tempLoadData;
            if (!ParseGSSog(fileDataArray.GetData().data(), fileByteLen, tempLoadData, primitive, imgMgr, 0u, 0u,
                    *loadCtx.shuttingDown_)) {
                CORE_LOG_E("SOG: ParseGSSog failed for %s", gsPath.c_str());
                primitive.isValid = false;
                return primitive;
            }
            const size_t n = tempLoadData.gsVerticesData.size();
            loadData.gsVerticesData.Resize(n);
            for (size_t ii = 0; ii < n; ii++) {
                loadData.gsVerticesData.Set(ii, BASE_NS::move(tempLoadData.gsVerticesData[ii]));
            }

            loadData.sphericalHarmonics = BASE_NS::move(tempLoadData.sphericalHarmonics);
            loadData.gaussianPositions = BASE_NS::move(tempLoadData.gaussianPositions);
            loadData.gaussianIndices = BASE_NS::move(tempLoadData.gaussianIndices);
        } else if (formatType == PlyFormatType::Packed) {
            CORE_LOG_I("Loading compressed GS3D");
            ParseGSCompressedOld(gsHeader, headerEndIdx, fileDataArray.GetData().data(), &loadData,
                renderData->enableScoreCulling, primitive, loadCtx.threadPool_.get());
        } else {
            size_t numThreads = loadCtx.threadPool_->GetNumberOfThreads();
            if (numThreads == 0u) {
                numThreads = 1u;
            }

            BASE_NS::vector<IThreadPool::IResult::Ptr> results;
            results.reserve(numThreads);

            const size_t chunkSize = numGaussians / numThreads;
            std::mutex aabbSyncMtx;
            
            for (size_t ii = 0; ii < numThreads; ii++) {
                const size_t start = ii * chunkSize;
                const size_t end = (ii == numThreads - 1) ? numGaussians : start + chunkSize;

                auto task = CreateFunctionTask([&, start, end]() {
                    BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max() };
                    BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };

                    for (size_t ii = start; ii < end; ii++) {
                        ParseGSVertex(formatType, ii, headerEndIdx, fileDataArray.GetData().data(), &loadData,
                            renderData->enableScoreCulling, primitive, aabbMin, aabbMax);
                    }
                    std::lock_guard<std::mutex> lock(aabbSyncMtx);
                    primitive.aabbMin = min(primitive.aabbMin, aabbMin);
                    primitive.aabbMax = max(primitive.aabbMax, aabbMax);
                });

                results.push_back(loadCtx.threadPool_->Push(BASE_NS::move(task)));
            }

            for (auto& result : results) {
                result->Wait();
            }
        }
        fileDataArray = ByteArray(0);
    }

    if (loadCtx.shuttingDown_->load()) { // check exit flag
        CORE_LOG_I("GS async load cancelled.");
        return {};
    }

    {
        const auto& primitives = loadCtx.dataStoreGs_->GetGsPrimitives();
        primitive.startOffset =
            primitives.empty() ? 0 : (primitives.back().startOffset + primitives.back().gsBufferSize);
        primitive.gsBufferSize = static_cast<uint32_t>(loadData.gsVerticesData.Size());
        if (primitives.empty()) {
            primitive.startShOffset = 0;
        } else if (primitives.back().RequireSH()) {
            // meaning that the previous one needs sh buffer
            primitive.startShOffset = primitives.back().startShOffset + primitives.back().gsBufferSize;
        } else {
            primitive.startShOffset = primitives.back().startShOffset;
        }
    }

    // setting transparentCullingOpacityAbsolute to > 1.0f can be used to force culling on
    if ((primitive.numGs > GS_CULLING_THRESHOLD || dfc.transparentCullingOpacityAbsolute > 1.0f) &&
        dfc.useAutomaticCulling) {
        CullTransparentGaussians(primitive, &loadData, loadCtx.threadPool_, dfc.useAutomaticCulling,
            dfc.transparentCullingOpacityAbsolute, loadCtx);
    }

    if (loadCtx.shuttingDown_->load()) { // check exit flag
        CORE_LOG_I("GS async load cancelled.");
        return {};
    }

    // lodbias > 2.0f can be used to force LODs always on for all models.
    if ((dfc.computeLods && numGaussians > GS_LOD_MIN_NUM_GS) || (dfc.computeLods && dfc.lodBias > 2.0f)) {
        CreateDynamicLODs(primitive, &loadData, loadCtx.threadPool_);
    }

    if (loadCtx.shuttingDown_->load()) { // check exit flag
        CORE_LOG_I("GS async load cancelled.");
        return {};
    }

    primitive.gsBufferSize = primitive.numGs + primitive.numGs1;
    CORE_LOG_D("A new primitive added with start offset of %d and size of %d (%d + %d)", primitive.startOffset,
        primitive.gsBufferSize, primitive.numGs, primitive.numGs1);

    primitive.computeLods =
        (dfc.computeLods && primitive.numGs > GS_LOD_MIN_NUM_GS) || (dfc.computeLods && dfc.lodBias > 2.0f);

    renderData->anyComputeLods = primitive.computeLods;
    for (const auto& prim : loadCtx.dataStoreGs_->GetGsPrimitives()) {
        renderData->anyComputeLods = renderData->anyComputeLods || prim.computeLods;
    }

    // compute splat extents for raycasting support
    if (renderData->enableGsPicking) {
        loadData.splatExtents.resize(primitive.numGs + primitive.numGs1);
        for (size_t sIdx = 0; sIdx < loadData.gaussianPositions.size(); sIdx++) {
            const GsObjectLoadData::GsSplatData& splatData = loadData.gsVerticesData.Get(sIdx);
            loadData.splatExtents[sIdx] = ComputeSplatExtent(splatData.covAScale, splatData.covBColorPacked, 1.0f);
        }
    }
#ifdef __OHOS_PLATFORM_PARSING_CAMERA_POSE_ENABLED__
    bool enableEdit = loadCtx.gsConfigComponent_ ? loadCtx.gsConfigComponent_->enableGsEditor : false;
    if (enableEdit) {
        dfc.gsPosData.resize(numGaussians * 4);
        for (uint32_t i = 0; i < numGaussians; i++) {
            dfc.gsPosData[i * 4] = loadData.gsVerticesData.Get(i).positionOpacity.x;
            dfc.gsPosData[i * 4 + 1] = loadData.gsVerticesData.Get(i).positionOpacity.y;
            dfc.gsPosData[i * 4 + 2] = loadData.gsVerticesData.Get(i).positionOpacity.z;
            dfc.gsPosData[i * 4 + 3] = loadData.gsVerticesData.Get(i).positionOpacity.w;
        }
    }
#endif
    // transfer from GsObjectLoadDataExt to the GPU and GsRenderDatas
    {
        size_t totalNumberOfSplats = primitive.numGs + primitive.numGs1;
        size_t totalNumberIndexable = primitive.numGs;
        size_t writeStartOffset = 0;
        size_t totalSplatsNeedSh = primitive.RequireSH() ? primitive.gsBufferSize : 0;
        for (const auto& prim : loadCtx.dataStoreGs_->GetGsPrimitives()) {
            totalNumberOfSplats += prim.numGs + prim.numGs1;
            totalNumberIndexable += prim.numGs;
            writeStartOffset += prim.numGs + prim.numGs1;
            totalSplatsNeedSh += prim.RequireSH() ? prim.gsBufferSize : 0;
        }

        // create the new buffers
        CreateGsRenderDataBuffers(renderData, totalNumberOfSplats, totalNumberIndexable, totalSplatsNeedSh, loadCtx);

        // add the newly created to the end of the buffers through a staging buffer

        // splat data
        {
            size_t splatDataSize = sizeof(GsObjectLoadData::GsSplatData);
            if (renderData->CanUsePackedData()) {
                splatDataSize = sizeof(GsObjectLoadData::GsSplatDataPacked);
            }
            const uint32_t vertexByteSize = (primitive.numGs + primitive.numGs1) * static_cast<uint32_t>(splatDataSize);
            const BufferCopy vertexCopy {
                0,                                                       // srcOffset
                writeStartOffset * static_cast<uint32_t>(splatDataSize), // dstOffset
                vertexByteSize,                                          // size
            };

            if (loadCtx.dataStoreGs_->GetGsPrimitives().size() == 0) {
                renderData->gsSplatData = loadData.gsVerticesData.FinishGPUAlloc();
            } else {
                auto vertexStagingHandle = loadData.gsVerticesData.FinishGPUAlloc();
                if (!RenderHandleUtil::IsValid(vertexStagingHandle.GetHandle())) {
                    CORE_LOG_E("gpu buffer not valid during render");
                }
                loadCtx.dsStaging_->CopyBufferToBuffer(vertexStagingHandle, renderData->gsSplatData, vertexCopy,
                    IRenderDataStoreDefaultStaging::ResourceCopyInfo::BEGIN_FRAME);
            }
        }

        // harmonics data
        if (primitive.RequireSH()) {
            const uint32_t harmonicsByteSize = (primitive.numGs + primitive.numGs1) * sizeof(GS::GsHarmonicsData);
            auto harmonicsStagingHandle = gpuResourceMgr.Create(GetStagingBufferDesc(harmonicsByteSize),
                BASE_NS::array_view<uint8_t>((uint8_t*)loadData.sphericalHarmonics.data(),
                    static_cast<uint32_t>(loadData.sphericalHarmonics.size()) * sizeof(GS::GsHarmonicsData)));

            if (!RenderHandleUtil::IsValid(harmonicsStagingHandle.GetHandle())) {
                CORE_LOG_E("Failed to create harmonics staging buffer");
                return primitive;
            }

            const BufferCopy harmonicsCopy {
                0,                                                                            // srcOffset
                primitive.startShOffset * static_cast<uint32_t>(sizeof(GS::GsHarmonicsData)), // dstOffset
                harmonicsByteSize,                                                            // size
            };

            loadCtx.dsStaging_->CopyBufferToBuffer(harmonicsStagingHandle, renderData->gsHarmonicsDataRef,
                harmonicsCopy, IRenderDataStoreDefaultStaging::ResourceCopyInfo::BEGIN_FRAME);
        }

        const auto lg = static_cast<RenderDataStoreDefaultGS*>(loadCtx.dataStoreGs_.get())->GetDataLockGuard();
        if (loadCtx.dataStoreGs_->GetGsPrimitives().size() > 0) {
            // splat extents
            if (renderData->enableGsPicking) {
                std::copy(loadData.splatExtents.begin(), loadData.splatExtents.end(),
                    renderData->splatExtents.begin() + writeStartOffset);
            }

            // position data
            std::copy(loadData.gaussianPositions.begin(), loadData.gaussianPositions.end(),
                renderData->gaussianPositions.begin() + writeStartOffset);

            // max scales
            if (renderData->enableScoreCulling) {
                std::copy(loadData.gaussianMaxScales.begin(), loadData.gaussianMaxScales.end(),
                    renderData->gaussianMaxScales.begin() + writeStartOffset);
            }
        } else {
            // splat extents
            if (renderData->enableGsPicking) {
                renderData->splatExtents = BASE_NS::move(loadData.splatExtents);
            }

            // position data
            renderData->gaussianPositions = BASE_NS::move(loadData.gaussianPositions);

            // max scales
            if (renderData->enableScoreCulling) {
                renderData->gaussianMaxScales = BASE_NS::move(loadData.gaussianMaxScales);
            }
        }
        primitive.isValid = true;
    }

    primitive.isValid = true;
    return primitive;
}

RENDER_NS::RenderHandleReference GS::CreateGSBuffer(IGpuResourceManager& gpuResourceMgr, const string_view& name,
    const uint32_t byteSize, const array_view<const uint8_t>& data, const BufferUsageFlags bufferUsageFlags,
    const RENDER_NS::MemoryPropertyFlags memoryProperties)
{
    GpuBufferDesc desc;
    desc.usageFlags = bufferUsageFlags;

    desc.memoryPropertyFlags = memoryProperties;

    desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
    desc.byteSize = byteSize;
    RENDER_NS::RenderHandleReference rhr = gpuResourceMgr.Create(name, desc, data);
    return rhr;
}

void GS::SetDefaultASTC(
    GS::GsRenderData* renderData, RENDER_NS::IGpuResourceManager& gpuResourceMgr, const GS::GsLoadContext loadCtx)
{
    const int maxTexNum = GS_MAX_NUM_GS_TEXTURES;

    if (renderData->gsShASTCDataRefs.size() < maxTexNum) {
        GpuImageDesc descImg;
        descImg.usageFlags =
            ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;
        descImg.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
        descImg.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        descImg.engineCreationFlags = 0;
        descImg.width = 4u;
        descImg.height = 4u;
        descImg.format = Format::BASE_FORMAT_R8G8B8A8_UNORM;
        BASE_NS::vector<uint8_t> shASTCData;
        shASTCData.resize(16u);
        renderData->gsShASTCDataRefs.resize(maxTexNum);
        renderData->astcSizes.resize(maxTexNum);

        for (size_t ii = 0; ii < maxTexNum; ii++) {
            renderData->gsShASTCDataRefs[ii] = gpuResourceMgr.Create(descImg,
                BASE_NS::array_view<uint8_t>((uint8_t*)(shASTCData.data()), shASTCData.size() * sizeof(uint8_t)));

            renderData->astcSizes[ii] = Math::UVec2(4u, 4u);
        }
    }
}

void GS::CreateGsRenderDataBuffers(GS::GsRenderData* renderData, const size_t totalNumberOfSplats,
    const size_t totalNumberIndexable, const size_t totalSplatsNeedSh, GS::GsLoadContext loadCtx)
{
    if (!renderData) {
        return;
    }

    /**
     *  Create new GS buffers and transfer the previous GS data into it contiguously.
     */

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::UpdateOrCreateGsDataStorage", "", 0xFF00FF);
    if (!loadCtx.renderContext_ || !loadCtx.multiGsBufferIndexCounter || !loadCtx.dsStaging_ || !loadCtx.dataStoreGs_) {
        return;
    }
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();

    const auto getvalidSize = [](const uint32_t byteSize) -> uint32_t { return Math::max(byteSize, 16u); };

    bool useHostVisibleMemory = true;
    if (renderData->CanUseGpuSort()) {
        useHostVisibleMemory = renderData->anyComputeLods;
    }

    const MemoryPropertyFlags memoryFlags =
        useHostVisibleMemory
            ? static_cast<MemoryPropertyFlags>(MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            : static_cast<MemoryPropertyFlags>(MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    GpuBufferDesc desc;
    desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                      BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT |
                      BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT;
    desc.memoryPropertyFlags = memoryFlags;
    desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

    // index buffers
    {
        uint64_t indexByteSize64 = sizeof(uint32_t) * static_cast<uint64_t>(totalNumberIndexable);
        if (indexByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        desc.byteSize = getvalidSize(static_cast<uint32_t>(indexByteSize64));
    }
    const auto bufferIdx = BASE_NS::to_string(*loadCtx.multiGsBufferIndexCounter);
    auto gsIndicesRef = gpuResourceMgr.Create(
        "GS3DIndices_" + bufferIdx + "_" + loadCtx.uniqueName, desc);
    if (!gsIndicesRef) {
        CORE_LOG_E("Failed to create index buffer");
        return;
    }
    auto gsIndicesRef2 = gpuResourceMgr.Create(
        "GS3DIndices2_" + bufferIdx + "_" + loadCtx.uniqueName, desc);
    if (!gsIndicesRef2) {
        CORE_LOG_E("Failed to create index buffer 2");
        return;
    }

    // vertex data
    size_t gsSplatSize = renderData->CanUsePackedData() ? sizeof(GsObjectLoadData::GsSplatDataPacked)
                                                        : sizeof(GsObjectLoadData::GsSplatData);
    {
        uint64_t splatByteSize64 = gsSplatSize * totalNumberOfSplats;
        if (splatByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        desc.byteSize = getvalidSize(static_cast<uint32_t>(splatByteSize64));
    }
    desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT |
                      BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT;

    if (loadCtx.dataStoreGs_->GetGsPrimitives().size() == 0) {
        // no buffer to buffer copy for the first model in GPU BUFFER ONLY mode
        desc.byteSize = getvalidSize(0u);
    }

    auto gsSplatData = gpuResourceMgr.Create(
        "GS3DSplatData_" + bufferIdx + "_" + loadCtx.uniqueName, desc);

    if (!renderData->enableCustomAnimation) {
        desc.byteSize = getvalidSize(0u);
    } else {
        uint64_t splatByteSize64Render = gsSplatSize * totalNumberOfSplats;
        if (splatByteSize64Render > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        desc.byteSize = getvalidSize(static_cast<uint32_t>(splatByteSize64Render));
    }
    auto gsSplatDataRender = gpuResourceMgr.Create(
        "GS3DSplatDataRender_" + bufferIdx + "_" + loadCtx.uniqueName, desc);

    // harmonics data
    {
        uint64_t harmonicsByteSize64 = sizeof(GS::GsHarmonicsData) * static_cast<uint64_t>(totalSplatsNeedSh);
        if (harmonicsByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        desc.byteSize = getvalidSize(static_cast<uint32_t>(harmonicsByteSize64));
    }
    auto gsHarmonicsDataRef = gpuResourceMgr.Create(
        "GS3DSphericalHarmonics_" + bufferIdx + "_" + loadCtx.uniqueName, desc);

    // recreate the GS frame data precompute storage
    {
        GpuBufferDesc precomputeDesc;
        precomputeDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                    BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        precomputeDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        precomputeDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        {
            uint64_t precompByteSize64 = static_cast<uint64_t>(totalNumberIndexable) * sizeof(Math::UVec4);
            if (precompByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
                CORE_LOG_E("Buffer size overflow");
                return;
            }
            precomputeDesc.byteSize = getvalidSize(static_cast<uint32_t>(precompByteSize64));
        }
        const bool keepTiledCacheHandles = renderData->isTiled && renderData->tilePool;
        auto dataStorageCacheHandle = gpuResourceMgr.Create(
            "GS3DDataStorageCache_" + bufferIdx + "_" + loadCtx.uniqueName, precomputeDesc);
        if (!keepTiledCacheHandles) {
            renderData->dataStorageCacheHandle = dataStorageCacheHandle;
        }

        GpuBufferDesc viewZDesc;
        viewZDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                               BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        viewZDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        viewZDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        if (renderData->enableFrameGeneration) {
            viewZDesc.byteSize = getvalidSize(static_cast<uint32_t>(totalNumberIndexable) * sizeof(float));
        } else {
            // small and empty buffer if frame generation is not enabled
            viewZDesc.byteSize = getvalidSize(1u * sizeof(float));
        }
        renderData->viewZStoreCacheHandle = gpuResourceMgr.Create(
            "GS3DViewZStoreCache_" + bufferIdx + "_" + loadCtx.uniqueName, viewZDesc);
    }

    // Transfer the previous GS data into the new buffers.
    // This could be done in 1 copy if a primitive was added and at most 2 copies if a
    // primitive was removed, but this was easier to implement and the overhead is negligible.
    uint32_t vertexWriteOffset = 0;
    uint32_t harmonicsWriteOffset = 0;
    uint32_t positionsWriteOffset = 0;

    const auto lg = static_cast<RenderDataStoreDefaultGS*>(loadCtx.dataStoreGs_.get())->GetDataLockGuard();
    BASE_NS::vector<BASE_NS::Math::Vec4> gaussianPositions;
    BASE_NS::vector<float> splatExtents;
    BASE_NS::vector<float> gaussianMaxScales;
    if (loadCtx.dataStoreGs_->GetGsPrimitives().size() > 0) {
        gaussianPositions.resize(totalNumberOfSplats);
        if (renderData->enableGsPicking) {
            splatExtents.resize(totalNumberOfSplats);
        }
        if (renderData->enableScoreCulling) {
            gaussianMaxScales.resize(totalNumberOfSplats);
        }
    }

    for (const GS::GsPrimitive& prim : loadCtx.dataStoreGs_->GetGsPrimitives()) {
        const uint32_t readOffsetCount = prim.startOffset;
        const uint32_t elementCount = prim.numGs + prim.numGs1;
        if (elementCount == 0u) {
            continue;
        }

        // guard against drifted per-primitive offsets
        if (static_cast<size_t>(readOffsetCount) + elementCount > renderData->gaussianPositions.size() ||
            static_cast<size_t>(positionsWriteOffset) + elementCount > gaussianPositions.size() ||
            (renderData->enableGsPicking &&
                (static_cast<size_t>(readOffsetCount) + elementCount > renderData->splatExtents.size() ||
                    static_cast<size_t>(positionsWriteOffset) + elementCount > splatExtents.size())) ||
            (renderData->enableScoreCulling &&
                (static_cast<size_t>(readOffsetCount) + elementCount > renderData->gaussianMaxScales.size() ||
                    static_cast<size_t>(positionsWriteOffset) + elementCount > gaussianMaxScales.size()))) {
            CORE_LOG_E("GS render buffer copy range out of bounds (read %u + %u, write %u); skipping primitive.",
                readOffsetCount, elementCount, positionsWriteOffset);
            continue;
        }

        // copy GS vertex data
        const uint32_t vertexCopyByteSize = elementCount * static_cast<uint32_t>(gsSplatSize);
        const BufferCopy vertexCopy {
            static_cast<uint32_t>(readOffsetCount * sizeof(GsObjectLoadData::GsSplatData)), // srcOffset
            vertexWriteOffset,                                                              // dstOffset
            vertexCopyByteSize,                                                             // size
        };

        loadCtx.dsStaging_->CopyBufferToBuffer(renderData->gsSplatData, gsSplatData, vertexCopy,
            IRenderDataStoreDefaultStaging::ResourceCopyInfo::BEGIN_FRAME);
        vertexWriteOffset += vertexCopyByteSize;

        // copy GS harmonics data
        if (prim.RequireSH()) {
            const uint32_t harmonicsCopyByteSize = elementCount * sizeof(GS::GsHarmonicsData);
            const BufferCopy harmonicsCopy {
                static_cast<uint32_t>(prim.startShOffset * sizeof(GS::GsHarmonicsData)), // srcOffset
                harmonicsWriteOffset,                                                    // dstOffset
                harmonicsCopyByteSize,                                                   // size
            };

            loadCtx.dsStaging_->CopyBufferToBuffer(renderData->gsHarmonicsDataRef, gsHarmonicsDataRef, harmonicsCopy,
                IRenderDataStoreDefaultStaging::ResourceCopyInfo::BEGIN_FRAME);
            harmonicsWriteOffset += harmonicsCopyByteSize;
        }

        // copy CPU-side position data for CPU sorting
        std::copy(renderData->gaussianPositions.begin() + readOffsetCount,
            renderData->gaussianPositions.begin() + readOffsetCount + elementCount,
            gaussianPositions.begin() + positionsWriteOffset);

        // copy splat extents for raycast support
        if (renderData->enableGsPicking) {
            std::copy(renderData->splatExtents.begin() + readOffsetCount,
                renderData->splatExtents.begin() + readOffsetCount + elementCount,
                splatExtents.begin() + positionsWriteOffset);
        }

        // copy maxScale data
        if (renderData->enableScoreCulling) {
            std::copy(renderData->gaussianMaxScales.begin() + readOffsetCount,
                renderData->gaussianMaxScales.begin() + readOffsetCount + elementCount,
                gaussianMaxScales.begin() + positionsWriteOffset);
        }

        positionsWriteOffset += elementCount;
    }

    // resize index data to match
    renderData->gaussianIndices.resize(totalNumberIndexable);
    std::fill(renderData->gaussianIndices.begin(), renderData->gaussianIndices.end(), 0u);

    if (loadCtx.dataStoreGs_->GetGsPrimitives().size() > 0) {
        renderData->gaussianPositions = gaussianPositions;
        if (renderData->enableScoreCulling) {
            renderData->gaussianMaxScales = gaussianMaxScales;
        }
        if (renderData->enableGsPicking) {
            renderData->splatExtents = splatExtents;
        }
    }

    // move the buffers to renderData
    renderData->gsIndicesRef = gsIndicesRef;
    renderData->gsIndicesRef2 = gsIndicesRef2;
    renderData->gsSplatData = gsSplatData;
    renderData->gsSplatDataRender = gsSplatDataRender;
    renderData->gsHarmonicsDataRef = gsHarmonicsDataRef;

    // populate byte sizes
    {
        uint64_t dataCopyByteSize64 = gsSplatSize * totalNumberOfSplats;
        if (dataCopyByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        renderData->gsDataCopyRefByteSize = static_cast<uint32_t>(dataCopyByteSize64);
    }
    {
        uint64_t harmonicsRefByteSize64 = sizeof(GS::GsHarmonicsData) * static_cast<uint64_t>(totalSplatsNeedSh);
        if (harmonicsRefByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Buffer size overflow");
            return;
        }
        renderData->gsHarmonicsDataRefByteSize = getvalidSize(static_cast<uint32_t>(harmonicsRefByteSize64));
    }

    // GPU sort buffers
    if (renderData->CanUseGpuSort()) {
        if (renderData->anyComputeLods) {
            CORE_LOG_W("GS3D GPU sort is not currently supported with the LOD system, defaulting to CPU sort.");
            renderData->useGpuSort = false;
        } else {
            GpuBufferDesc sortDesc;
            sortDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            sortDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            sortDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

            // sort key-indices buffer
            {
                uint64_t sortByteSize64 = sizeof(SortKeyIndex) * static_cast<uint64_t>(totalNumberOfSplats);
                if (sortByteSize64 > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
                    CORE_LOG_E("Buffer size overflow");
                    return;
                }
                sortDesc.byteSize = getvalidSize(static_cast<uint32_t>(sortByteSize64));
            }
            // ping
            renderData->gpuSortBuffers.sortKeyIndexBuffer0 = gpuResourceMgr.Create(
                "GS3DSortKeyIndexBuffer0_" + bufferIdx + "_" + loadCtx.uniqueName, sortDesc);
            // pong
            renderData->gpuSortBuffers.sortKeyIndexBuffer1 = gpuResourceMgr.Create(
                "GS3DSortKeyIndexBuffer1_" + bufferIdx + "_" + loadCtx.uniqueName, sortDesc);

            // sort count buffer
            const uint32_t globalInvocationSize =
                (static_cast<uint32_t>(totalNumberOfSplats) / NUM_BLOCKS_PER_GROUP) +
                (static_cast<uint32_t>(totalNumberOfSplats) % NUM_BLOCKS_PER_GROUP > 0 ? 1 : 0);
            const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;

            sortDesc.byteSize = getvalidSize(static_cast<uint32_t>(sizeof(uint32_t) * RADIX_SORT_BINS * numWorkGroups));
            renderData->gpuSortBuffers.sortCountBuffer =
                gpuResourceMgr.Create("GS3DSortCountBuffer_" + bufferIdx + "_" + loadCtx.uniqueName,
                    sortDesc);
        }
    }

    // tile pool GPU buffers
    if (renderData->isTiled && renderData->tilePool && renderData->tilePool->maxTiles > 0) {
        GS::GsTilePool& pool = *renderData->tilePool;

        const auto getvalid = [](const uint32_t v) -> uint32_t { return Math::max(v, 16u); };
        const auto checkedByteSize = [&](const uint64_t bytes, const char* name, uint32_t& out) -> bool {
            if (bytes > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                CORE_LOG_E("GS tile pool allocation '%s' is too large: %llu bytes", name,
                    static_cast<unsigned long long>(bytes));
                return false;
            }
            out = getvalid(static_cast<uint32_t>(bytes));
            return true;
        };

        const size_t splatDataStride = sizeof(GsObjectLoadData::GsSplatData);
        const uint64_t legacySplatCapacity64 = static_cast<uint64_t>(pool.maxTiles) * Math::max(pool.splatsPerTile, 1u);
        if (legacySplatCapacity64 > std::numeric_limits<uint32_t>::max()) {
            CORE_LOG_E("GS tile pool legacy capacity is too large: %llu splats",
                static_cast<unsigned long long>(legacySplatCapacity64));
            return;
        }

        pool.splatCapacity = Math::max(pool.splatCapacity, static_cast<uint32_t>(legacySplatCapacity64));
        pool.sortCapacity = Math::min(Math::max(pool.sortCapacity, 1024u), pool.splatCapacity);

        BASE_NS::string poolNameSuffix(loadCtx.uniqueName.c_str());
        poolNameSuffix += "_";
        poolNameSuffix += BASE_NS::to_string(pool.resourceVersion);

        uint32_t byteSize = 0u;

        if (!RENDER_NS::RenderHandleUtil::IsValid(pool.splatPoolBuffer.GetHandle()) ||
            pool.allocatedSplatCapacity < pool.splatCapacity) {
            const RENDER_NS::RenderHandleReference oldSplatPoolBuffer = pool.splatPoolBuffer;
            const uint32_t oldSplatCapacity = pool.allocatedSplatCapacity;

            GpuBufferDesc poolDesc;
            poolDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            poolDesc.engineCreationFlags =
                EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS |
                EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE;
            poolDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT;

            if (!checkedByteSize(static_cast<uint64_t>(pool.splatCapacity) * splatDataStride, "splatPool", byteSize)) {
                return;
            }
            poolDesc.byteSize = byteSize;
            pool.splatPoolBuffer = gpuResourceMgr.Create("GS3DTileSplatPool_" + poolNameSuffix, poolDesc);

            if (RENDER_NS::RenderHandleUtil::IsValid(oldSplatPoolBuffer.GetHandle()) && oldSplatCapacity > 0u &&
                oldSplatPoolBuffer.GetHandle() != pool.splatPoolBuffer.GetHandle()) {
                const uint64_t copyBytes64 =
                    static_cast<uint64_t>(Math::min(oldSplatCapacity, pool.splatCapacity)) * splatDataStride;
                if (copyBytes64 > 0u) {
                    void* oldMapped = gpuResourceMgr.MapBufferMemory(oldSplatPoolBuffer);
                    void* newMapped = gpuResourceMgr.MapBufferMemory(pool.splatPoolBuffer);
                    if (oldMapped && newMapped) {
                        BASE_NS::CloneData(
                            newMapped, static_cast<size_t>(copyBytes64), oldMapped, static_cast<size_t>(copyBytes64));
                    }

                    if (newMapped) {
                        gpuResourceMgr.UnmapBuffer(pool.splatPoolBuffer);
                    }
                    if (oldMapped) {
                        gpuResourceMgr.UnmapBuffer(oldSplatPoolBuffer);
                    }
                }
            }

            pool.allocatedSplatCapacity = pool.splatCapacity;
        }

        const bool needSortPool = !RENDER_NS::RenderHandleUtil::IsValid(pool.sortIndexPool0.GetHandle()) ||
                                  !RENDER_NS::RenderHandleUtil::IsValid(pool.sortIndexPool1.GetHandle()) ||
                                  !RENDER_NS::RenderHandleUtil::IsValid(pool.sortCountPoolBuffer.GetHandle()) ||
                                  !RENDER_NS::RenderHandleUtil::IsValid(pool.tileDataStorageCacheHandle.GetHandle()) ||
                                  !RENDER_NS::RenderHandleUtil::IsValid(pool.tileViewZStoreCacheHandle.GetHandle()) ||
                                  pool.allocatedSortCapacity < pool.sortCapacity;

        if (needSortPool) {
            GpuBufferDesc sortDesc;
            sortDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            sortDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            sortDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT;

            if (!checkedByteSize(
                    static_cast<uint64_t>(pool.sortCapacity) * sizeof(SortKeyIndex), "sortIndex", byteSize)) {
                return;
            }
            sortDesc.byteSize = byteSize;
            pool.sortIndexPool0 = gpuResourceMgr.Create("GS3DTileSortIndexPool0_" + poolNameSuffix, sortDesc);
            pool.sortIndexPool1 = gpuResourceMgr.Create("GS3DTileSortIndexPool1_" + poolNameSuffix, sortDesc);

            {
                const uint32_t globalInvocationSize =
                    (pool.sortCapacity + NUM_BLOCKS_PER_GROUP_RADIX - 1u) / NUM_BLOCKS_PER_GROUP_RADIX;
                const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1u)) / RADIX_SORT_BINS;
                if (!checkedByteSize(
                        static_cast<uint64_t>(sizeof(uint32_t)) * RADIX_SORT_BINS * Math::max(numWorkGroups, 1u),
                        "sortCount", byteSize)) {
                    return;
                }
                pool.sortCountSlotSize = byteSize;

                GpuBufferDesc countPoolDesc;
                countPoolDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                countPoolDesc.engineCreationFlags =
                    EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
                countPoolDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                countPoolDesc.byteSize = byteSize;
                pool.sortCountPoolBuffer =
                    gpuResourceMgr.Create("GS3DTileSortCountPool_" + poolNameSuffix, countPoolDesc);
            }

            {
                GpuBufferDesc tileCacheDesc;
                tileCacheDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                           BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                tileCacheDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                tileCacheDesc.engineCreationFlags =
                    EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
                if (!checkedByteSize(
                        static_cast<uint64_t>(pool.sortCapacity) * sizeof(Math::UVec4), "tileDataCache", byteSize)) {
                    return;
                }
                tileCacheDesc.byteSize = byteSize;
                pool.tileDataStorageCacheHandle =
                    gpuResourceMgr.Create("GS3DDataStorageCache_tile_" + poolNameSuffix, tileCacheDesc);

                GpuBufferDesc viewZDesc;
                viewZDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                viewZDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                viewZDesc.engineCreationFlags =
                    EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
                if (renderData->enableFrameGeneration) {
                    if (!checkedByteSize(
                            static_cast<uint64_t>(pool.sortCapacity) * sizeof(float), "tileViewZCache", byteSize)) {
                        return;
                    }
                    viewZDesc.byteSize = byteSize;
                } else {
                    viewZDesc.byteSize = getvalid(sizeof(float));
                }

                pool.tileViewZStoreCacheHandle =
                    gpuResourceMgr.Create("GS3DViewZStoreCache_tile_" + poolNameSuffix, viewZDesc);
            }

            pool.allocatedSortCapacity = pool.sortCapacity;
        }

        const bool needTileBuffers = !RENDER_NS::RenderHandleUtil::IsValid(pool.tileMetaBuffer.GetHandle()) ||
                                     !RENDER_NS::RenderHandleUtil::IsValid(pool.tileIndirectDrawBuffer.GetHandle()) ||
                                     pool.allocatedTileCapacity < pool.maxTiles;
        if (needTileBuffers) {
            GpuBufferDesc tileDesc;
            tileDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            tileDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            tileDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (!checkedByteSize(
                    static_cast<uint64_t>(pool.maxTiles) * sizeof(BASE_NS::Math::Vec4) * 2u, "tileMeta", byteSize)) {
                return;
            }
            tileDesc.byteSize = byteSize;
            pool.tileMetaBuffer = gpuResourceMgr.Create("GS3DTileMetaUBO_" + poolNameSuffix, tileDesc);

            tileDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            tileDesc.byteSize = getvalid(static_cast<uint32_t>(sizeof(GsIndirectDrawDispatch)));
            pool.tileIndirectDrawBuffer =
                gpuResourceMgr.Create("GS3DTileIndirectDrawBuffer_" + poolNameSuffix, tileDesc);

            pool.allocatedTileCapacity = pool.maxTiles;
        }

        CORE_LOG_I("GS tile pool allocated: maxTiles=%u, splatCapacity=%u, sortCapacity=%u (%.1f MB splat pool).",
            pool.maxTiles, pool.splatCapacity, pool.sortCapacity,
            static_cast<float>(static_cast<uint64_t>(pool.splatCapacity) * splatDataStride) / (1024.0f * 1024.0f));
    }

    renderData->areResourcesLoaded = false;

    (*loadCtx.multiGsBufferIndexCounter)++;
}

GS::GsPrimitive GS::LoadGsPrimitive(GsComponent& dfc, Entity splatEntity, GS::GsLoadContext loadCtx)
{
    if (!loadCtx.dataStoreGs_ || !loadCtx.renderContext_ || !loadCtx.wmcm_ || !loadCtx.tempRenderData) {
        return {};
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::LoadGsPrimitive", "", 0xFFFF00);

    const auto loadBeginTime = std::chrono::high_resolution_clock::now();

    if (dfc.gsScaleFactor == 0) {
        CORE_LOG_E("gsScaleFactor cannot be 0, aborting!");
        return {};
    }

    GS::GsRenderData* renderData = loadCtx.tempRenderData;

    GsPrimitive prim = LoadGaussianData(dfc.gsPath, renderData, dfc, splatEntity, loadCtx);
    prim.splatEntity = splatEntity;

    if (!prim.isValid) {
        CORE_LOG_E("Invalid GS model, aborting!");
        return {};
    }

    if (loadCtx.wmcm_->HasComponent(prim.splatEntity)) {
        prim.worldMatrix = loadCtx.wmcm_->Read(prim.splatEntity)->matrix;
    }

    // set animation data
    {
        const auto loadEndTime = std::chrono::high_resolution_clock::now();

        const CORE_NS::EngineTime timeSinceStart = loadCtx.renderContext_->GetEngine().GetEngineTime();
        const float timeSinceStartSeconds =
            static_cast<float>(timeSinceStart.totalTimeUs) / 1000000.0f; // micros to seconds

        GsAnimationFadeProperties animationProperties;
        animationProperties.fadeSpeed = dfc.fadeSpeed;
        animationProperties.useFadeAnimation = dfc.useFadeAnimation && (animationProperties.fadeSpeed > 0.0f);
        animationProperties.fadeWaitOffset =
            static_cast<float>(std::chrono::duration_cast<std::chrono::seconds>(loadEndTime - loadBeginTime).count()) +
            timeSinceStartSeconds + dfc.fadeWaitOffset;
        animationProperties.timeToFadeFullPoints = dfc.timeToFadeFullPoints;

        animationProperties.fadeStartPosition = dfc.fadeInStartPosition;
        animationProperties.fadeDistanceOffset = 0.0;

        prim.animationFadeProperties = animationProperties;
    }

    // set crop-box data
    {
        GsCropBoxProperties cropProp;
        cropProp.aabbMin = dfc.cropBoxAABBMin;
        cropProp.aabbMax = dfc.cropBoxAABBMax;
        cropProp.useCropBox = dfc.gsUseCropBox;

        prim.cropBoxProperties = cropProp;
    }

#ifndef GS_USE_CROPBOX
    if (dfc.gsUseCropBox) {
        CORE_LOG_E("CropBox usage is requested but GS_USE_CROPBOX is disabled in gs_common.h!");
    }
#endif

    loadCtx.dataStoreGs_->SetGsScaleFactor(dfc.gsScaleFactor);
    renderData->gsEnableDepthPass = renderData->gsEnableDepthPass || dfc.gsEnableDepthPass;
    prim.isValid = true;
    renderData->convertGsColorSrgb = renderData->convertGsColorSrgb || dfc.convertGsColorSrgb;
    renderData->use16bitSurfaceTarget = renderData->use16bitSurfaceTarget || dfc.use16bitSurfaceTarget;

    return BASE_NS::move(prim);
}