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

#ifndef PLUGIN_GS_LOADING_H
#define PLUGIN_GS_LOADING_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <numeric>
#include <plugin_gs/ecs/components/gs_component.h>
#include <plugin_gs/ecs/components/gs_configuration_component.h>
#include <plugin_gs/implementation_uids.h>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <plugin_gs/util/intf_gs_picking.h>
#include <random>
#include <thread>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/post_process_configuration_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/render_handle_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_animation_system.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <base/containers/array_view.h>
#include <base/containers/byte_array.h>
#include <base/containers/fixed_string.h>
#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/json/json.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_render_post_processes.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>

#include "gs_vertex_blob.h"

namespace GS {
// per-object data storage for loading the GS objects
struct GsObjectLoadDataExt {
    // these will be thrown away after load
    BASE_NS::vector<GsHarmonicsData> sphericalHarmonics;
    GsVertexBlob gsVerticesData;

    // these will be moved to GsRenderData
    BASE_NS::vector<BASE_NS::Math::Vec4> gaussianPositions;
    BASE_NS::vector<uint32_t> gaussianIndices;
    BASE_NS::vector<float> splatExtents;
    BASE_NS::vector<float> gaussianMaxScales;
};

struct GsLoadContext {
    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    CORE3D_NS::IWorldMatrixComponentManager* wmcm_ { nullptr };
    CORE3D_NS::IRenderMeshComponentManager* renderMeshManager_ { nullptr };

    CORE_NS::IThreadPool::Ptr threadPool_;
    CORE_NS::IEcs* ecs_ { nullptr };

    GsRenderData* tempRenderData { nullptr };

    BASE_NS::fixed_string<21u> uniqueName;
    BASE_NS::refcnt_ptr<GS::IRenderDataStoreDefaultGs> dataStoreGs_;
    RENDER_NS::IRenderDataStoreDefaultStaging* dsStaging_ { nullptr };

    std::shared_ptr<uint32_t> multiGsBufferIndexCounter { nullptr };

    std::atomic_bool* shuttingDown_ { nullptr };
    BASE_NS::shared_ptr<GsConfigurationComponent> gsConfigComponent_ { nullptr };
};

float ComputeSplatExtent(const BASE_NS::Math::Vec3 covA, const BASE_NS::Math::Vec3 covB, const float stdDevMultiplier);

GS::GsPrimitive LoadGaussianData(const BASE_NS::string& gsPath, GS::GsRenderData* renderData, GS::GsComponent& dfc,
    CORE_NS::Entity splatEntity, const GsLoadContext loadCtx);

void CreateGsRenderTargets(GS::GsRenderData* renderData, CORE_NS::Entity targetEntity, const GS::GsLoadContext loadCtx,
    const BASE_NS::Math::UVec2 resolution, const bool resolutionRecreate = false);

void RemoveGsRenderTargets(GS::GsRenderData* renderData, CORE_NS::Entity targetEntity, const GS::GsLoadContext loadCtx);

RENDER_NS::RenderHandleReference CreateGSBuffer(RENDER_NS::IGpuResourceManager& gpuResourceMgr,
    const BASE_NS::string_view& name, const uint32_t byteSize, const BASE_NS::array_view<const uint8_t>& data,
    const RENDER_NS::BufferUsageFlags bufferUsageFlags, const RENDER_NS::MemoryPropertyFlags memoryProperties);

void SetDefaultASTC(
    GS::GsRenderData* renderData, RENDER_NS::IGpuResourceManager& gpuResourceMgr, const GS::GsLoadContext loadCtx);

void CreateGsRenderDataBuffers(GS::GsRenderData* renderData, const size_t totalNumberOfSplats,
    const size_t totalNumberIndexable, const size_t totalSplatsNeedSh, GS::GsLoadContext loadCtx);

GS::GsPrimitive LoadGsPrimitive(GS::GsComponent& dfc, CORE_NS::Entity splatEntity, GS::GsLoadContext loadCtx);

} // namespace GS

#endif