# Lume 3D engine rendering demo — Native CMake
cmake_minimum_required(VERSION 3.5.0)
project(LumeApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# Engine source root (relative to this CMakeLists.txt → ../../../../../lume)
get_filename_component(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../lume" ABSOLUTE)

# ---------- securec shim (provides memcpy_s/memmove_s/memset_s) ----------
add_library(securec_shim INTERFACE)
target_include_directories(securec_shim INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
)

# ---------- LumeBase (header-only) ----------
add_library(LumeBase INTERFACE)
target_include_directories(LumeBase INTERFACE
    ${ENGINE_ROOT}/LumeBase/api
)
target_link_libraries(LumeBase INTERFACE securec_shim)

# ---------- LumeEngine (shared library) ----------
set(LUME_ENGINE_DIR ${ENGINE_ROOT}/LumeEngine)

add_library(LumeEngine SHARED
    # -- Platform-agnostic sources --
    ${LUME_ENGINE_DIR}/src/ecs/ecs.cpp
    ${LUME_ENGINE_DIR}/src/ecs/entity_manager.cpp
    ${LUME_ENGINE_DIR}/src/engine.cpp
    ${LUME_ENGINE_DIR}/src/engine_factory.cpp
    ${LUME_ENGINE_DIR}/src/image/image_loader_manager.cpp
    ${LUME_ENGINE_DIR}/src/image/loaders/image_loader_astc.cpp
    ${LUME_ENGINE_DIR}/src/image/loaders/image_loader_ktx.cpp
    ${LUME_ENGINE_DIR}/src/io/filesystem_api.cpp
    ${LUME_ENGINE_DIR}/src/io/file_manager.cpp
    ${LUME_ENGINE_DIR}/src/io/memory_file.cpp
    ${LUME_ENGINE_DIR}/src/io/memory_filesystem.cpp
    ${LUME_ENGINE_DIR}/src/io/path_tools.cpp
    ${LUME_ENGINE_DIR}/src/io/proxy_directory.cpp
    ${LUME_ENGINE_DIR}/src/io/proxy_filesystem.cpp
    ${LUME_ENGINE_DIR}/src/io/rofs_filesystem.cpp
    ${LUME_ENGINE_DIR}/src/io/std_directory.cpp
    ${LUME_ENGINE_DIR}/src/io/std_file.cpp
    ${LUME_ENGINE_DIR}/src/io/std_filesystem.cpp
    ${LUME_ENGINE_DIR}/src/io/dev/file_monitor.cpp
    ${LUME_ENGINE_DIR}/src/loader/system_graph_loader.cpp
    ${LUME_ENGINE_DIR}/src/log/logger.cpp
    ${LUME_ENGINE_DIR}/src/log/logger_output.cpp
    ${LUME_ENGINE_DIR}/src/perf/performance_data_manager.cpp
    ${LUME_ENGINE_DIR}/src/plugin_registry.cpp
    ${LUME_ENGINE_DIR}/src/threading/dispatcher_impl.cpp
    ${LUME_ENGINE_DIR}/src/threading/dispatcher_task_queue.cpp
    ${LUME_ENGINE_DIR}/src/threading/parallel_impl.cpp
    ${LUME_ENGINE_DIR}/src/threading/parallel_task_queue.cpp
    ${LUME_ENGINE_DIR}/src/threading/sequential_impl.cpp
    ${LUME_ENGINE_DIR}/src/threading/sequential_task_queue.cpp
    ${LUME_ENGINE_DIR}/src/threading/task_queue.cpp
    ${LUME_ENGINE_DIR}/src/threading/task_queue_factory.cpp
    ${LUME_ENGINE_DIR}/src/util/frustum_util.cpp
    ${LUME_ENGINE_DIR}/src/version.cpp

    # -- OHOS NDK platform sources (rawfile + filesystem + platform init) --
    # Upstream dropped the NAPI-based "ohos_ndk" platform implementation in
    # favour of the full-tree "ohos" one (C++ ResourceManager, not shippable
    # against the app SDK). The app keeps the NDK implementation, restored
    # verbatim from the pre-sync tree; only library_ohos.cpp is shared.
    ${LUME_ENGINE_DIR}/src/os/ohos/library_ohos.cpp
    ${LUME_ENGINE_DIR}/src/os/ohos_ndk/logger_output_ndk.cpp
    ${LUME_ENGINE_DIR}/src/os/ohos_ndk/ohos_file_ndk.cpp
    ${LUME_ENGINE_DIR}/src/os/ohos_ndk/ohos_filesystem_ndk.cpp
    ${LUME_ENGINE_DIR}/src/os/ohos_ndk/platform_ohos_ndk.cpp
    ${LUME_ENGINE_DIR}/src/os/elf/process_elf.cpp

    # -- ECS helper sources --
    ${LUME_ENGINE_DIR}/ecshelper/ComponentTools/component_query.cpp
    ${LUME_ENGINE_DIR}/api/core/property_tools/property_data.cpp
)

target_include_directories(LumeEngine PUBLIC
    ${LUME_ENGINE_DIR}/api
    ${LUME_ENGINE_DIR}/api/platform/ohos_ndk
PRIVATE
    ${LUME_ENGINE_DIR}/src
    ${LUME_ENGINE_DIR}/ecshelper
)

target_compile_definitions(LumeEngine PUBLIC
    __OHOS_PLATFORM__
    __OHOS__
    BUILD_PUBLIC_VERSION
    CORE_BUILD_BASE=1
    CORE_HIDE_SYMBOLS=1
    CORE_PERF_ENABLED=0
    CORE_USE_COMPILER_GENERATED_STATIC=0
    CORE_USE_COMPILER_GENERATED_STATIC_LIST=0
    CORE_DEV_ENABLED=1
    CORE_LOG_TO_CONSOLE=1
    CORE_LOG_TO_DEBUG_OUTPUT=1
    CORE_LOG_TO_FILE=0
    CORE_VALIDATION_ENABLED=1
    CORE_DYNAMIC=0
    CORE_EMBEDDED_ASSETS_ENABLED=0
    USE_STB_IMAGE=0
    USE_LIB_PNG_JPEG=0
    CORE_PUBLIC=__attribute__\(\(visibility\("default"\)\)\)
    LIB_ENGINE_CORE=libAGPDLL
    LIB_RENDER=libPluginAGPRender
    LIB_CORE3D=libPluginAGP3D
)

target_compile_options(LumeEngine PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Export plugin registry symbols so plugin .so files can call RegisterStaticPlugin
set_source_files_properties(
    ${LUME_ENGINE_DIR}/src/plugin_registry.cpp
    PROPERTIES COMPILE_FLAGS "-fvisibility=default"
)

target_link_libraries(LumeEngine PUBLIC
    LumeBase
    securec_shim
    libhilog_ndk.z.so
    librawfile.z.so
    libace_napi.z.so
)

# ---------- LumeRenderApi (header-only, INTERFACE) ----------
add_library(LumeRenderApi INTERFACE)
target_include_directories(LumeRenderApi INTERFACE
    ${ENGINE_ROOT}/LumeRender/api
)

# ---------- LumeMeta (shared library) ----------
set(LUME_META_DIR ${ENGINE_ROOT}/LumeMeta)

add_library(LumeMeta SHARED
    ${LUME_META_DIR}/src/animation/animation_controller.cpp
    ${LUME_META_DIR}/src/animation/animation_state.cpp
    ${LUME_META_DIR}/src/animation/interpolator.cpp
    ${LUME_META_DIR}/src/animation/keyframe_animation.cpp
    ${LUME_META_DIR}/src/animation/modifiers/loop.cpp
    ${LUME_META_DIR}/src/animation/modifiers/reverse.cpp
    ${LUME_META_DIR}/src/animation/modifiers/speed.cpp
    ${LUME_META_DIR}/src/animation/parallel_animation.cpp
    ${LUME_META_DIR}/src/animation/property_animation.cpp
    ${LUME_META_DIR}/src/animation/sequential_animation.cpp
    ${LUME_META_DIR}/src/animation/staggered_animation_state.cpp
    ${LUME_META_DIR}/src/animation/track_animation.cpp
    ${LUME_META_DIR}/src/animation/track_animation_state.cpp
    ${LUME_META_DIR}/src/attachment_container.cpp
    ${LUME_META_DIR}/src/call_context.cpp
    ${LUME_META_DIR}/src/class_registry.cpp
    ${LUME_META_DIR}/src/connector.cpp
    ${LUME_META_DIR}/src/container/container.cpp
    ${LUME_META_DIR}/src/container/container_base.cpp
    ${LUME_META_DIR}/src/container/flat_container.cpp
    ${LUME_META_DIR}/src/container/object_container.cpp
    ${LUME_META_DIR}/src/container/object_flat_container.cpp
    ${LUME_META_DIR}/src/container_observer.cpp
    ${LUME_META_DIR}/src/content_object.cpp
    ${LUME_META_DIR}/src/curves/bezier_curve.cpp
    ${LUME_META_DIR}/src/curves/easing_curve.cpp
    ${LUME_META_DIR}/src/engine/engine_value.cpp
    ${LUME_META_DIR}/src/engine/engine_value_manager.cpp
    ${LUME_META_DIR}/src/functions.cpp
    ${LUME_META_DIR}/src/future.cpp
    ${LUME_META_DIR}/src/manual_clock.cpp
    ${LUME_META_DIR}/src/meta_object_lib.cpp
    ${LUME_META_DIR}/src/number.cpp
    ${LUME_META_DIR}/src/object_context.cpp
    ${LUME_META_DIR}/src/object_data_container.cpp
    ${LUME_META_DIR}/src/object_hierarchy_observer.cpp
    ${LUME_META_DIR}/src/object_registry.cpp
    ${LUME_META_DIR}/src/polling_task_queue.cpp
    ${LUME_META_DIR}/src/property/bind.cpp
    ${LUME_META_DIR}/src/property/dependencies.cpp
    ${LUME_META_DIR}/src/property/property.cpp
    ${LUME_META_DIR}/src/property/stack_property.cpp
    ${LUME_META_DIR}/src/proxy_object.cpp
    ${LUME_META_DIR}/src/random.cpp
    ${LUME_META_DIR}/src/ref_uri_util.cpp
    ${LUME_META_DIR}/src/register_anys.cpp
    ${LUME_META_DIR}/src/register_default_anims.cpp
    ${LUME_META_DIR}/src/register_default_objs.cpp
    ${LUME_META_DIR}/src/register_engine_access.cpp
    ${LUME_META_DIR}/src/register_entities.cpp
    ${LUME_META_DIR}/src/register_value_serializers.cpp
    ${LUME_META_DIR}/src/resource/animation_resource.cpp
    ${LUME_META_DIR}/src/resource/file_resource_manager.cpp
    ${LUME_META_DIR}/src/resource/object_resource.cpp
    ${LUME_META_DIR}/src/resource/object_template.cpp
    ${LUME_META_DIR}/src/serialization/backend/debug_output.cpp
    ${LUME_META_DIR}/src/serialization/backend/json_input.cpp
    ${LUME_META_DIR}/src/serialization/backend/json_output.cpp
    ${LUME_META_DIR}/src/serialization/exporter.cpp
    ${LUME_META_DIR}/src/serialization/importer.cpp
    ${LUME_META_DIR}/src/serialization/json_exporter.cpp
    ${LUME_META_DIR}/src/serialization/json_importer.cpp
    ${LUME_META_DIR}/src/serialization/metav1_compat.cpp
    ${LUME_META_DIR}/src/serialization/refuri_builder.cpp
    ${LUME_META_DIR}/src/startable_object_controller.cpp
    ${LUME_META_DIR}/src/system_clock.cpp
    ${LUME_META_DIR}/src/threaded_task_queue.cpp
)

target_include_directories(LumeMeta
    PUBLIC  ${LUME_META_DIR}/include
    PRIVATE ${LUME_META_DIR}/src
)

target_compile_definitions(LumeMeta PUBLIC
    META_PUBLIC=__attribute__\(\(visibility\("default"\)\)\)
)

target_compile_options(LumeMeta PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-private-field
    -Wno-implicit-fallthrough
    -Wno-reorder
    -Wno-unused-function
    -Wno-unused-lambda-capture
    -Wno-unused-but-set-variable
    -Wno-overloaded-virtual
    -Wno-missing-braces
)

target_link_libraries(LumeMeta PUBLIC
    LumeEngine
    LumeRenderApi
    LumeBase
    securec_shim
)

# ---------- Lume_3D (shared library) ----------
set(LUME_3D_DIR ${ENGINE_ROOT}/Lume_3D)

add_library(Lume_3D SHARED
    # -- ECS helper (each .so needs its own copy, matching GN build) --
    ${LUME_ENGINE_DIR}/ecshelper/ComponentTools/component_query.cpp
    ${LUME_ENGINE_DIR}/api/core/property_tools/property_data.cpp
    ${LUME_3D_DIR}/version.cpp
    ${LUME_3D_DIR}/src/util/json.cpp
    ${LUME_3D_DIR}/src/plugin/static_plugin.cpp
    ${LUME_3D_DIR}/src/plugin/static_registry.cpp
    ${LUME_3D_DIR}/src/ecs/components/animation_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/animation_input_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/animation_output_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/animation_state_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/animation_track_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/camera_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/dynamic_environment_blender_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/environment_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/fog_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/graphics_state_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/initial_transform_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/joint_matrices_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/layer_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/light_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/light_probe_group_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/local_matrix_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/material_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/mesh_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/morph_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/name_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/node_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/physical_camera_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/planar_reflection_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/post_process_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/post_process_configuration_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/post_process_effect_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/previous_joint_matrices_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/reflection_probe_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/render_configuration_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/render_handle_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/render_mesh_batch_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/render_mesh_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/rsdz_model_id_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/skin_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/skin_ibm_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/skin_joints_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/transform_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/uri_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/water_ripple_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/weather_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/components/world_matrix_component_manager.cpp
    ${LUME_3D_DIR}/src/ecs/systems/animation_playback.cpp
    ${LUME_3D_DIR}/src/ecs/systems/animation_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/light_probe_baker_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/local_matrix_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/morphing_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/node_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/render_preprocessor_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/render_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/skinning_system.cpp
    ${LUME_3D_DIR}/src/ecs/systems/weather_system.cpp
    ${LUME_3D_DIR}/src/gltf/gltf2.cpp
    ${LUME_3D_DIR}/src/gltf/gltf2_exporter.cpp
    ${LUME_3D_DIR}/src/gltf/gltf2_importer.cpp
    ${LUME_3D_DIR}/src/gltf/gltf2_loader.cpp
    ${LUME_3D_DIR}/src/gltf/gltf2_util.cpp
    ${LUME_3D_DIR}/src/graphics_context.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_default_camera.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_default_light.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_default_material.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_default_scene.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_light_probe.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_morph.cpp
    ${LUME_3D_DIR}/src/render/datastore/render_data_store_weather.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_camera_single_post_process.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_camera_weather.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_create_default_camera_gpu_images.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_cameras.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_camera_controller.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_camera_post_process_controller.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_camera_post_process_interface_controller.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_env.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_environment_blender.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_light_probes.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_lights.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_material_deferred_shading.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_material_objects.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_material_render_slot.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_shadows_blur.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_default_shadow_render_slot.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_generics.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_light_probes_env.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_morph.cpp
    ${LUME_3D_DIR}/src/render/node/render_node_weather_simulation.cpp
    ${LUME_3D_DIR}/src/render/render_node_scene_util.cpp
    ${LUME_3D_DIR}/src/util/bowyer_watson_delaunay_3d.cpp
    ${LUME_3D_DIR}/src/util/light_probe_util.cpp
    ${LUME_3D_DIR}/src/util/mesh_builder.cpp
    ${LUME_3D_DIR}/src/util/mesh_util.cpp
    ${LUME_3D_DIR}/src/util/picking.cpp
    ${LUME_3D_DIR}/src/util/property_util.cpp
    ${LUME_3D_DIR}/src/util/render_util.cpp
    ${LUME_3D_DIR}/src/util/scene_util.cpp
    ${LUME_3D_DIR}/src/util/uri_lookup.cpp
)

target_include_directories(Lume_3D
    PUBLIC  ${LUME_3D_DIR}/api
    PRIVATE ${LUME_3D_DIR}/src
            ${LUME_3D_DIR}/assets
            ${ENGINE_ROOT}/LumeEngine/ecshelper
        ${ENGINE_ROOT}/LumeEngine/src
)

target_compile_definitions(Lume_3D PUBLIC
    CORE3D_PUBLIC=__attribute__\(\(visibility\("default"\)\)\)
    CORE3D_SHARED_LIBRARY=1
    CORE3D_VALIDATION_ENABLED=1
    CORE3D_TESTS_ENABLED=0
    CORE3D_DEV_ENABLED=1
    CORE3D_EMBEDDED_ASSETS_ENABLED=0
    CORE_USE_COMPILER_GENERATED_STATIC_LIST=0
    RENDER_HAS_VULKAN_BACKEND=1
    RENDER_HAS_GLES_BACKEND=1
)

target_compile_options(Lume_3D PRIVATE
    "-DCORE_STATIC_PLUGIN_HEADER=\"static_plugin_decl.h\""
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wno-braced-scalar-init
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-private-field
    -Wno-tautological-unsigned-zero-compare
    -Wno-logical-op-parentheses
    -Wno-thread-safety-attributes
)

target_link_libraries(Lume_3D PUBLIC
    LumeEngine
    LumeRenderApi
    LumeMeta
    LumeBase
    securec_shim
)

# ---------- LumeRender (shared library) ----------
set(LUME_RENDER_DIR ${ENGINE_ROOT}/LumeRender)

add_library(LumeRender SHARED
    # -- ECS helper (each .so needs its own copy) --
    ${LUME_ENGINE_DIR}/api/core/property_tools/property_data.cpp
    # -- Core sources --
    ${LUME_RENDER_DIR}/version.cpp
    ${LUME_RENDER_DIR}/src/plugin/static_plugin.cpp
    ${LUME_RENDER_DIR}/src/plugin/static_registry.cpp
    ${LUME_RENDER_DIR}/src/util/json.cpp
    ${LUME_RENDER_DIR}/src/util/native_fence_fd.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_default_acceleration_structure_staging.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_default_gpu_resource_data_copy.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_default_staging.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_manager.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_pod.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_post_process.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_render_post_processes.cpp
    ${LUME_RENDER_DIR}/src/datastore/render_data_store_shader_passes.cpp
    ${LUME_RENDER_DIR}/src/device/device.cpp
    ${LUME_RENDER_DIR}/src/device/gpu_program_util.cpp
    ${LUME_RENDER_DIR}/src/device/gpu_resource_cache.cpp
    ${LUME_RENDER_DIR}/src/device/gpu_resource_handle_util.cpp
    ${LUME_RENDER_DIR}/src/device/gpu_resource_manager.cpp
    ${LUME_RENDER_DIR}/src/device/gpu_resource_util.cpp
    ${LUME_RENDER_DIR}/src/device/shader_manager.cpp
    ${LUME_RENDER_DIR}/src/device/shader_pipeline_binder.cpp
    ${LUME_RENDER_DIR}/src/device/shader_reflection_data.cpp
    ${LUME_RENDER_DIR}/src/loader/pipeline_layout_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/render_data_configuration_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/render_data_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/render_node_graph_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/shader_data_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/shader_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/shader_state_loader.cpp
    ${LUME_RENDER_DIR}/src/loader/shader_state_loader_util.cpp
    ${LUME_RENDER_DIR}/src/loader/vertex_input_declaration_loader.cpp
    ${LUME_RENDER_DIR}/src/node/core_render_node_factory.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_back_buffer.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_bloom.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_combined_post_process.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_compute_generic.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_create_gpu_buffers.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_create_gpu_images.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_default_acceleration_structure_staging.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_end_frame_staging.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_fullscreen_generic.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_queue_transfer.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_gaussian_splatting.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_render_post_processes_generic.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_shader_passes_generic.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_single_post_process.cpp
    ${LUME_RENDER_DIR}/src/node/render_node_staging.cpp
    ${LUME_RENDER_DIR}/src/node/render_staging.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/node_context_descriptor_set_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/node_context_pso_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/pipeline_descriptor_set_binder.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_barrier_list.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_command_list.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_context_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_copy_util.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_graph_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_graph_share_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_manager.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_parser_util.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_post_process_util.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_post_process_interface_util.cpp
    ${LUME_RENDER_DIR}/src/nodecontext/render_node_util.cpp
    ${LUME_RENDER_DIR}/src/perf/cpu_timer.cpp
    ${LUME_RENDER_DIR}/src/perf/gpu_query_manager.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_bloom_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_blur_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_cmaa2_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_combined_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_dof_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_flare_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_fxaa_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_motion_blur_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_ssao_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_taa_node.cpp
    ${LUME_RENDER_DIR}/src/postprocesses/render_post_process_upscale_node.cpp
    ${LUME_RENDER_DIR}/src/renderer.cpp
    ${LUME_RENDER_DIR}/src/render_context.cpp
    ${LUME_RENDER_DIR}/src/render_graph.cpp
    ${LUME_RENDER_DIR}/src/resource_handle_impl.cpp
    ${LUME_RENDER_DIR}/src/util/property_util.cpp
    ${LUME_RENDER_DIR}/src/util/render_frame_util.cpp
    ${LUME_RENDER_DIR}/src/util/render_util.cpp
    ${LUME_RENDER_DIR}/src/util/shader_saver.cpp

    # -- Vulkan backend (19 files) --
    ${LUME_RENDER_DIR}/src/vulkan/create_functions_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/device_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_buffer_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_image_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_memory_allocator_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_program_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_query_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_resource_util_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_sampler_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/gpu_semaphore_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/node_context_descriptor_set_manager_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/node_context_pool_manager_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/pipeline_create_functions_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/pipeline_state_object_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/platform_hardware_buffer_util_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/render_backend_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/render_frame_sync_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/shader_module_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/swapchain_vk.cpp

    # -- Vulkan OHOS platform (6 files) --
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_create_functions_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_device_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_gpu_buffer_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_gpu_image_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_hardware_buffer_util_vk.cpp
    ${LUME_RENDER_DIR}/src/vulkan/ohos/platform_vk.cpp

    # -- GLES backend (18 files) --
    ${LUME_RENDER_DIR}/src/gles/device_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/egl_state.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_buffer_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_image_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_program_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_query_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_resource_util_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_sampler_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/gpu_semaphore_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/node_context_descriptor_set_manager_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/node_context_pool_manager_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/pipeline_state_object_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/render_backend_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/render_frame_sync_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/shader_module_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/spirv_cross_helpers_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/swapchain_gles.cpp
    ${LUME_RENDER_DIR}/src/gles/generic/platform_egl_state.cpp

    # -- GLES OHOS platform (1 file) --
    ${LUME_RENDER_DIR}/src/gles/ohos/platform_device_gles.cpp

    # -- Device NDK variant (1 file, used INSTEAD of device/ohos/) --
    ${LUME_RENDER_DIR}/src/device/ohos_ndk/platform_hardware_buffer.cpp
)

# Fix GLES OHOS platform file: __OHOS_PLATFORM__ (propagated from LumeEngine)
# makes it pick internal SDK headers (<external_window.h>, <surface/native_buffer.h>)
# which are absent in the NDK sysroot. Undefine the macro for this single file so
# the #else path selects the NDK headers (<native_window/external_window.h>, etc.).
set_source_files_properties(
    ${LUME_RENDER_DIR}/src/gles/ohos/platform_device_gles.cpp
    PROPERTIES COMPILE_FLAGS "-U__OHOS_PLATFORM__"
)

target_include_directories(LumeRender
    PUBLIC
        ${LUME_RENDER_DIR}/api
    PRIVATE
        ${LUME_RENDER_DIR}/src
        ${LUME_RENDER_DIR}/src/vulkan/ohos
        ${LUME_RENDER_DIR}/src/gles/ohos
        ${LUME_RENDER_DIR}/3rdparty
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
        ${ENGINE_ROOT}/LumeEngine/ecshelper
        ${ENGINE_ROOT}/LumeEngine/src
)

# CORE_STATIC_PLUGIN_HEADER must expand to a quoted #include path
target_compile_options(LumeRender PRIVATE
    "-DCORE_STATIC_PLUGIN_HEADER=\"static_plugin_decl.h\""
)

target_compile_definitions(LumeRender PUBLIC
    RENDER_PUBLIC=__attribute__\(\(visibility\("default"\)\)\)
    RENDER_HAS_VULKAN_BACKEND=1
    RENDER_HAS_GLES_BACKEND=1
    RENDER_HAS_MALEOON_BACKEND=0
    RENDER_EMBEDDED_ASSETS_ENABLED=0
    RENDER_VALIDATION_ENABLED=0
    RENDER_DEV_ENABLED=0
    RENDER_TESTS_ENABLED=0
    VMA_STATS_STRING_ENABLED=0
    VMA_STATIC_VULKAN_FUNCTIONS=1
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
    VK_USE_PLATFORM_OHOS=1
)

# App-only bridge for the legacy tiled LOD render node (product builds do not
# define this).
target_compile_definitions(LumeRender PRIVATE LUMEAPP_ENABLE_LEGACY_GS_NODE=1)

# VMA implementation must be compiled in exactly one .cpp in LumeRender
set_source_files_properties(
    ${LUME_RENDER_DIR}/src/vulkan/gpu_memory_allocator_vk.cpp
    PROPERTIES COMPILE_DEFINITIONS "VMA_IMPLEMENTATION"
)

target_compile_options(LumeRender PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-c++11-narrowing
)

target_link_libraries(LumeRender PUBLIC
    LumeEngine
    LumeBase
    securec_shim
    libvulkan.so
    libEGL.so
    libGLESv3.so
    libnative_buffer.so
    libnative_window.so
    libhilog_ndk.z.so
)


# ---------- LumeGS (official Gaussian splatting plugin, shared library) ----------
set(LUME_GS_DIR ${ENGINE_ROOT}/LumeGS)
add_library(LumeGS SHARED
    ${LUME_GS_DIR}/src/component_dll.cpp
    ${LUME_GS_DIR}/src/components/gs_component.cpp
    ${LUME_GS_DIR}/src/components/gs_configuration_component.cpp
    ${LUME_GS_DIR}/src/components/gs_tiling_component.cpp
    ${LUME_GS_DIR}/src/components/gs_effect_component.cpp
    ${LUME_GS_DIR}/src/data/sog_parsing.cpp
    ${LUME_GS_DIR}/src/data/glb_parsing.cpp
    ${LUME_GS_DIR}/src/decoder/gs_decoder.cpp
    ${LUME_GS_DIR}/src/loader/gs_loader.cpp
    ${LUME_GS_DIR}/src/loader/ai_edit_loader.cpp
    ${LUME_GS_DIR}/src/loader/gs_importer.cpp
    ${LUME_GS_DIR}/src/loader/spatial_codec_loader.cpp
    ${LUME_GS_DIR}/src/loader/xengine_loader.cpp
    ${LUME_GS_DIR}/src/render/render_data_store_default_gs.cpp
    ${LUME_GS_DIR}/src/render/render_data_store_gs_edit.cpp
    ${LUME_GS_DIR}/src/render/render_data_store_gs_floater_v2_1.cpp
    ${LUME_GS_DIR}/src/render/render_node_gs_render.cpp
    ${LUME_GS_DIR}/src/render/render_node_gs_floater_v2_1.cpp
    ${LUME_GS_DIR}/src/render/gsedit/gs_editor.cpp
    ${LUME_GS_DIR}/src/render/gsedit/gsedit_shader_action_executor.cpp
    ${LUME_GS_DIR}/src/render/gsedit/gsedit_shader_default_actions.cpp
    ${LUME_GS_DIR}/src/render/GsAIEdit/render_data_store_gs_ai_edit.cpp
    ${LUME_GS_DIR}/src/render/GsAIEdit/render_node_gs_ai_edit_render.cpp
    ${LUME_GS_DIR}/src/render/coloredit/render_effects_coloredit_node.cpp
    ${LUME_GS_DIR}/src/render/comic/render_effects_comic_node.cpp
    ${LUME_GS_DIR}/src/render/obradinn/render_effects_obradinn_node.cpp
    ${LUME_GS_DIR}/src/render/retro/render_effects_retro_node.cpp
    ${LUME_GS_DIR}/src/render/watercolor/render_effects_watercolor_node.cpp
    ${LUME_GS_DIR}/src/systems/gs_system.cpp
    ${LUME_GS_DIR}/src/systems/gs_tiling_system.cpp
    ${LUME_GS_DIR}/src/systems/gs_edit_system.cpp
    ${LUME_GS_DIR}/src/systems/gs_ai_edit_system.cpp
    ${LUME_GS_DIR}/src/systems/gs_effects_system.cpp
    ${LUME_GS_DIR}/src/util/gs_loading.cpp
    ${LUME_GS_DIR}/src/util/gs_picking.cpp
    ${LUME_GS_DIR}/src/util/gs_serialization.cpp
    ${LUME_GS_DIR}/src/util/gs_vertex_blob.cpp
    ${LUME_GS_DIR}/src/webp/image_loader_webp_imagekit.cpp
    ${LUME_GS_DIR}/version.cpp
    ${ENGINE_ROOT}/LumeEngine/api/core/property_tools/property_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/gs_rofs_embed.cpp
)

target_include_directories(LumeGS
    PUBLIC
        ${LUME_GS_DIR}/api
    PRIVATE
        ${LUME_GS_DIR}/src
        ${ENGINE_ROOT}/LumeEngine/src
        ${LUME_GS_DIR}/assets
        ${LUME_GS_DIR}/api/plugin_gs
        ${LUME_GS_DIR}/src/loader
        ${LUME_GS_DIR}/src/render
        ${LUME_GS_DIR}/src/loader/spatial_codec_api
        ${LUME_GS_DIR}/src/loader/xengine_api
        ${ENGINE_ROOT}/LumeEngine/ecshelper
        ${CMAKE_CURRENT_SOURCE_DIR}/shims
)

# Built in static-plugin mode like LumeRender/Lume_3D: the library registers
# itself through DEFINE_STATIC_PLUGIN() when it is loaded, so the app's
# LoadPlugins({}) picks it up without an explicit name lookup.
target_compile_options(LumeGS PRIVATE
    "-DCORE_STATIC_PLUGIN_HEADER=\"static_plugin_decl.h\""
)

target_compile_options(LumeGS PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-c++11-narrowing
)

# LumeApp: keep tiled LOD at full resolution (the legacy pipeline was full-res).
target_link_libraries(LumeGS PUBLIC
    LumeEngine
    LumeBase
    LumeMeta
    LumeRender
    Lume_3D
    securec_shim
    z
    libhilog_ndk.z.so
    libnative_window.so
    libnative_buffer.so
    libimage_source.so
    libpixelmap.so
)

# ---------- lumeapp: application .so linking all shared engine libs ----------
add_library(lumeapp SHARED
    render_main.cpp
    gaussian_splat_data.cpp
)

# Ensure gaussian_splat_data symbols are exported despite inherited -fvisibility=hidden
set_source_files_properties(gaussian_splat_data.cpp
    PROPERTIES COMPILE_FLAGS "-fvisibility=default"
)

target_link_libraries(lumeapp PUBLIC
    LumeEngine
    LumeGS
    LumeBase
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_buffer.so
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libhilog_ndk.z.so
    # SOG assets store their textures as WebP (decoded via the ImageKit NDK)
    libimage_source.so
    libpixelmap.so
)

# Include headers from Render/3D/Meta without linking (loaded via dlopen at runtime)
target_include_directories(lumeapp PRIVATE
    ${ENGINE_ROOT}/LumeRender/api
    ${ENGINE_ROOT}/Lume_3D/api
    ${ENGINE_ROOT}/LumeMeta/include
    ${ENGINE_ROOT}/LumeGS/api
)

# Allow unresolved symbols from Render/3D/Meta — they are resolved at runtime via dlopen
target_link_options(lumeapp PRIVATE -Wl,--allow-shlib-undefined)

# Copy ALL shared libraries (engine + app) to entry/libs so DevEco packages them into HAP
set(LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/arm64-v8a)
file(MAKE_DIRECTORY ${LIBS_DIR})
add_custom_command(TARGET lumeapp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:lumeapp> ${LIBS_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LumeEngine> ${LIBS_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LumeRender> ${LIBS_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Lume_3D> ${LIBS_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LumeMeta> ${LIBS_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:LumeGS> ${LIBS_DIR}/
    COMMENT "Copying all shared libraries to entry/libs for HAP packaging"
)
