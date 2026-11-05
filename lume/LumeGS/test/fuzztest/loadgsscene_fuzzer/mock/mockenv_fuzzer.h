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

#ifndef MOCKENV_FUZZER_H
#define MOCKENV_FUZZER_H

#include <cstdint>
#include <fstream>
#include <mutex>

#include <base/containers/array_view.h>
#include <base/containers/refcnt_ptr.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/vector.h>
#include <base/containers/unordered_map.h>
#include <base/math/quaternion.h>
#include <base/math/vector.h>
#include <base/namespace.h>
#include <base/util/uid.h>
#include <core/ecs/component_struct_macros.h>
#include <core/ecs/entity.h>
#include <core/ecs/entity_reference.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/ecs/intf_entity_manager.h>
#include <core/ecs/intf_system.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/namespace.h>
#include <core/property/property_types.h>
#include <core/threading/intf_thread_pool.h>
#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/namespace.h>
#include <plugin_gs/ecs/components/gs_component.h>
#include <core/property/intf_property_api.h>

BASE_BEGIN_NAMESPACE()
template<class T>
class array_view;
struct Uid;
BASE_END_NAMESPACE()

CORE_BEGIN_NAMESPACE()
class IClassFactory;
class IPropertyApi;
class IPropertyHandle;
struct ComponentManagerTypeInfo;
struct SystemTypeInfo;
CORE_END_NAMESPACE()

CORE3D_BEGIN_NAMESPACE()
CORE3D_END_NAMESPACE()

namespace MockFuzz {

class MockFile;
class MockFileManager;
class MockEntityManager;
class MockSceneNode;
class MockNodeSystem;
class MockECS;
class MockThreadPool;

template<typename ComponentType, const BASE_NS::Uid& UID>
class MockComponentManagerBase;

class MockUriComponentManager;
class MockGsComponentManager;

// ============================================================================
// Forward declarations for component types
// ============================================================================

using UriComponent = CORE3D_NS::UriComponent;
using GsComponent = GS::GsComponent;

// ============================================================================
// MockFile - In-memory file implementation
// ============================================================================

class MockFile : public CORE_NS::IFile {
public:
    MockFile() = default;
    ~MockFile() override { ::free(buf_); }
    MockFile(const MockFile&) = delete;
    MockFile& operator=(const MockFile&) = delete;

    Mode GetMode() const override;
    void Close() override;
    uint64_t Read(void* buffer, uint64_t count) override;

    uint64_t Write(const void* buffer, uint64_t count) override;
    uint64_t Append(const void* buffer, uint64_t count, uint64_t flushSize) override;
    uint64_t GetLength() const override;
    bool Seek(uint64_t offset) override;
    uint64_t GetPosition() const override;
    void Destroy() override;

private:
    uint8_t* buf_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
    size_t pos_ = 0;
};

// ============================================================================
// MockFileManager - In-memory file manager
// ============================================================================

class MockFileManager : public CORE_NS::IFileManager {
public:
    MockFileManager();
    ~MockFileManager() override;

    void Clear();

    // IInterface
    const CORE_NS::IInterface* GetInterface(const BASE_NS::Uid& uid) const override;
    CORE_NS::IInterface* GetInterface(const BASE_NS::Uid& uid) override;
    void Ref() noexcept override;
    void Unref() noexcept override;

    // IFileManager
    CORE_NS::IFile::Ptr OpenFile(BASE_NS::string_view uri) override;
    CORE_NS::IFile::Ptr OpenFile(BASE_NS::string_view uri, CORE_NS::IFile::Mode) override;
    CORE_NS::IFile::Ptr CreateFile(BASE_NS::string_view uri) override;
    bool DeleteFile(BASE_NS::string_view uri) override;
    bool FileExists(BASE_NS::string_view uri) const override;

    CORE_NS::IDirectory::Entry GetEntry(BASE_NS::string_view uri) override;
    CORE_NS::IDirectory::Ptr OpenDirectory(BASE_NS::string_view uri) override;
    CORE_NS::IDirectory::Ptr CreateDirectory(BASE_NS::string_view uri) override;
    bool DeleteDirectory(BASE_NS::string_view uri) override;
    bool DirectoryExists(BASE_NS::string_view uri) const override;
    bool Rename(BASE_NS::string_view fromUri, BASE_NS::string_view toUri) override;
    bool RegisterFilesystem(BASE_NS::string_view protocol, CORE_NS::IFilesystem::Ptr filesystem) override;
    void UnregisterFilesystem(BASE_NS::string_view protocol) override;
    void RegisterAssetPath(BASE_NS::string_view uri) override;
    void UnregisterAssetPath(BASE_NS::string_view uri) override;
    bool CheckExistence(BASE_NS::string_view protocol) override;
    bool RegisterPath(BASE_NS::string_view protocol, BASE_NS::string_view uri, bool prepend) override;
    void UnregisterPath(BASE_NS::string_view protocol, BASE_NS::string_view uri) override;
    CORE_NS::IFilesystem::Ptr CreateROFilesystem(const void* const data, uint64_t size) override;
    CORE_NS::IFilesystem* GetFilesystem(BASE_NS::string_view protocol) const override;

private:
    mutable std::mutex mutex_;
    BASE_NS::unordered_map<BASE_NS::string, MockFile*> files_;
    std::vector<MockFile*> allFiles_;
};

// ============================================================================
// MockEntityManager - Mock implementation of IEntityManager
// ============================================================================

class MockEntityManager : public CORE_NS::IEntityManager {
public:
    MockEntityManager();
    ~MockEntityManager() override = default;

    CORE_NS::EntityReference CreateReferenceCounted() override;

    CORE_NS::Entity Create() override;
    CORE_NS::EntityReference GetReferenceCounted(CORE_NS::Entity entity) override;
    void Destroy(const CORE_NS::Entity entity) override;
    void DestroyAllEntities() override;
    bool IsAlive(const CORE_NS::Entity entity) const override;
    void SetActive(const CORE_NS::Entity entity, bool state) override;
    uint32_t GetGenerationCounter() const override;
    Iterator::Ptr Begin(IteratorType type = IteratorType::ALIVE) const override;
    Iterator::Ptr End(IteratorType type = IteratorType::ALIVE) const override;

private:
    std::vector<CORE_NS::Entity> entities_;
    uint32_t idCounter_ = 1;
};

// ============================================================================
// MockSceneNode - Mock implementation of ISceneNode
// ============================================================================

class MockSceneNode : public CORE3D_NS::ISceneNode {
public:
    MockSceneNode(CORE_NS::Entity entity);
    ~MockSceneNode() override = default;

    CORE_NS::Entity GetEntity() const override;
    void SetParent(CORE3D_NS::ISceneNode const& node) override;
    CORE3D_NS::ISceneNode* GetParent() const override;

    BASE_NS::string GetName() const override;
    void SetName(BASE_NS::string_view name) override;
    void SetEnabled(bool isEnabled) override;
    bool GetEnabled() const override;
    bool GetEffectivelyEnabled() const override;
    bool IsAncestorOf(CORE3D_NS::ISceneNode const& node) override;
    BASE_NS::array_view<CORE3D_NS::ISceneNode* const> GetChildren() const override;
    BASE_NS::array_view<CORE3D_NS::ISceneNode*> GetChildren() override;
    const CORE3D_NS::ISceneNode* GetChild(BASE_NS::string_view const& name) const override;
    CORE3D_NS::ISceneNode* GetChild(BASE_NS::string_view const& name) override;
    bool AddChild(CORE3D_NS::ISceneNode& node) override;
    bool InsertChild(size_t index, CORE3D_NS::ISceneNode& node) override;
    bool RemoveChild(CORE3D_NS::ISceneNode& node) override;
    bool RemoveChild(size_t index) override;
    bool RemoveChildren() override;
    const CORE3D_NS::ISceneNode* LookupNodeByPath(BASE_NS::string_view const& path) const override;
    CORE3D_NS::ISceneNode* LookupNodeByPath(BASE_NS::string_view const& path) override;
    const CORE3D_NS::ISceneNode* LookupNodeByName(BASE_NS::string_view const& name) const override;
    CORE3D_NS::ISceneNode* LookupNodeByName(BASE_NS::string_view const& name) override;
    const CORE3D_NS::ISceneNode* LookupNodeByComponent(const CORE_NS::IComponentManager& componentManager) const override;
    CORE3D_NS::ISceneNode* LookupNodeByComponent(const CORE_NS::IComponentManager& componentManager) override;
    BASE_NS::vector<const CORE3D_NS::ISceneNode*> LookupNodesByComponent(const CORE_NS::IComponentManager& componentManager) const override;
    BASE_NS::vector<CORE3D_NS::ISceneNode*> LookupNodesByComponent(const CORE_NS::IComponentManager& componentManager) override;
    BASE_NS::Math::Vec3 GetPosition() const override;
    BASE_NS::Math::Quat GetRotation() const override;
    BASE_NS::Math::Vec3 GetScale() const override;
    void SetScale(const BASE_NS::Math::Vec3& scale) override;
    void SetPosition(const BASE_NS::Math::Vec3& position) override;
    void SetRotation(const BASE_NS::Math::Quat& rotation) override;
    uint32_t GetSceneId() const override;
    void SetSceneId(uint32_t sceneId) override;

private:
    CORE_NS::Entity entity_;
    const CORE3D_NS::ISceneNode* parent_ = nullptr;
    uint32_t sceneId_ = 0;
};

// ============================================================================
// MockNodeSystem - Mock implementation of INodeSystem
// ============================================================================

class MockNodeSystem : public CORE3D_NS::INodeSystem {
public:
    MockNodeSystem(CORE_NS::IEcs* ecs);
    ~MockNodeSystem() override = default;

    BASE_NS::string_view GetName() const override { return "NodeSystem"; }
    BASE_NS::Uid GetUid() const override { return BASE_NS::Uid("b564d740-3d39-41e7-9add-0bdbdbdf23a8"); }
    CORE_NS::IPropertyHandle* GetProperties() override { return nullptr; }
    const CORE_NS::IPropertyHandle* GetProperties() const override { return nullptr; }
    void SetProperties(const CORE_NS::IPropertyHandle&) override {}

    // INodeSystem
    CORE3D_NS::ISceneNode& GetRootNode() const override;
    CORE3D_NS::ISceneNode* GetNode(CORE_NS::Entity entity) const override;
    CORE3D_NS::ISceneNode* CreateNode() override;
    CORE3D_NS::ISceneNode* CloneNode(const CORE3D_NS::ISceneNode& node, bool recursive) override;
    void DestroyNode(CORE3D_NS::ISceneNode& node) override;
    void RefreshAllNodes() override;
    void AddListener(CORE3D_NS::INodeSystem::SceneNodeListener& listener) override;
    void RemoveListener(CORE3D_NS::INodeSystem::SceneNodeListener& listener) override;

    // ISystem
    bool IsActive() const override { return true; }
    void SetActive(bool isActive) override {}
    void Initialize() override {}
    bool Update(bool isFrameRenderingQueued, uint64_t time, uint64_t delta) override { return true; }
    void Uninitialize() override {}
    const CORE_NS::IEcs& GetECS() const override { return *ecs_; }

private:
    CORE_NS::IEcs* ecs_ = nullptr;
    std::vector<std::unique_ptr<CORE3D_NS::ISceneNode>> nodes_;
    std::unique_ptr<CORE3D_NS::ISceneNode> rootNode_;
};

// ============================================================================
// MockPropertyHandle - Simple IPropertyHandle implementation for mock
// ============================================================================
template<typename ComponentType, const BASE_NS::Uid& UID>
class MockPropertyHandle : public CORE_NS::IPropertyHandle {
public:
    explicit MockPropertyHandle(ComponentType* data) : data_(data) {}
    // IPropertyHandle interface implementing ONLY the actual virtual methods
    const CORE_NS::IPropertyApi* Owner() const override { 
        return nullptr; // Mock doesn't have a real Owner
    }
    
    size_t Size() const override {
        return sizeof(ComponentType);
    }
    const void* RLock() const override { 
        return data_; 
    }
    
    void RUnlock() const override {}
    
    void* WLock() override { 
        return data_; 
    }
    
    void WUnlock() override {}
private:
    ComponentType* data_;
};

class MockPropertyApi : public CORE_NS::IPropertyApi {
    size_t PropertyCount() const override { return 0; }
    const CORE_NS::Property* MetaData(size_t) const override { return nullptr; }
    BASE_NS::array_view<const CORE_NS::Property> MetaData() const override { return {}; }
    CORE_NS::IPropertyHandle* Create() const override { return nullptr; }
    CORE_NS::IPropertyHandle* Clone(const CORE_NS::IPropertyHandle*) const override { return nullptr; }
    void Release(CORE_NS::IPropertyHandle*) const override {}
    uint64_t Type() const override { return 0; }
};

// ============================================================================
// MockUriComponentManager - Mock implementation of IUriComponentManager
// ============================================================================
class MockUriComponentManager : public CORE3D_NS::IUriComponentManager {
public:
    MockUriComponentManager(CORE_NS::IEcs* ecs) : ecs_(ecs) {}
    ~MockUriComponentManager() override = default;
    // IComponentManager interface
    BASE_NS::string_view GetName() const override { return "MockUriComponentManager"; }
    BASE_NS::Uid GetUid() const override { return CORE3D_NS::IUriComponentManager::UID; }
    size_t GetComponentCount() const override {return 0;}
    const CORE_NS::IPropertyApi& GetPropertyApi() const override {return propertyApi_;}
    CORE_NS::Entity GetEntity(ComponentId index) const override {return CORE_NS::Entity {};}
    uint32_t GetComponentGeneration(ComponentId index) const override {return 0;}
    bool HasComponent(CORE_NS::Entity entity) const override;
    ComponentId GetComponentId(CORE_NS::Entity entity) const override {return 0;}
    void Create(CORE_NS::Entity entity) override;
    bool Destroy(CORE_NS::Entity entity) override;
    void Gc() override {}
    void Destroy(BASE_NS::array_view<const CORE_NS::Entity> gcList) override;
    BASE_NS::vector<CORE_NS::Entity> GetAddedComponents() override { return {}; }
    BASE_NS::vector<CORE_NS::Entity> GetRemovedComponents() override { return {}; }
    BASE_NS::vector<CORE_NS::Entity> GetUpdatedComponents() override { return {}; }
    CORE_NS::ComponentManagerModifiedFlags GetModifiedFlags() const override { return {}; }
    void ClearModifiedFlags() override {}
    uint32_t GetGenerationCounter() const override { return 0; }
    void SetData(CORE_NS::Entity entity, const CORE_NS::IPropertyHandle& data) override {}
    const CORE_NS::IPropertyHandle* GetData(CORE_NS::Entity entity) const override;
    CORE_NS::IPropertyHandle* GetData(CORE_NS::Entity entity) override;
    void SetData(ComponentId index, const CORE_NS::IPropertyHandle& data) override {}
    const CORE_NS::IPropertyHandle* GetData(ComponentId index) const override { return nullptr; }
    CORE_NS::IPropertyHandle* GetData(ComponentId index) override { return nullptr; }
    CORE_NS::IEcs& GetEcs() const override {return *ecs_; }
    BASE_NS::vector<CORE_NS::Entity> GetMovedComponents() override {return {}; }
    // IUriComponentManager generated interface from BEGIN_COMPONENT
    void Set(ComponentId index, const CORE3D_NS::UriComponent& data) override {}
    void Set(CORE_NS::Entity entity, const CORE3D_NS::UriComponent& data) override;
    CORE3D_NS::UriComponent Get(ComponentId index) const override {return CORE3D_NS::UriComponent{};}
    CORE3D_NS::UriComponent Get(CORE_NS::Entity entity) const override;
    CORE_NS::ScopedHandle<const CORE3D_NS::UriComponent> Read(ComponentId index) const override {return CORE_NS::ScopedHandle<const CORE3D_NS::UriComponent>();}
    CORE_NS::ScopedHandle<const CORE3D_NS::UriComponent> Read(CORE_NS::Entity entity) const override;
    CORE_NS::ScopedHandle<CORE3D_NS::UriComponent> Write(ComponentId index) override {return CORE_NS::ScopedHandle<CORE3D_NS::UriComponent>();}
    CORE_NS::ScopedHandle<CORE3D_NS::UriComponent> Write(CORE_NS::Entity entity) override;
private:
    CORE_NS::IEcs* ecs_ = nullptr;
    BASE_NS::unordered_map<uint32_t, CORE3D_NS::UriComponent> components_;
    uint32_t idCounter_ = 0;
    mutable std::mutex mutex_;
    mutable BASE_NS::unordered_map<uint32_t, std::unique_ptr<MockPropertyHandle<CORE3D_NS::UriComponent, CORE3D_NS::IUriComponentManager::UID>>> handles_;
    static MockPropertyApi propertyApi_;
};

// ============================================================================
// MockGsComponentManager - Mock implementation of IGsComponentManager
// ============================================================================
class MockGsComponentManager : public GS::IGsComponentManager {
public:
    MockGsComponentManager(CORE_NS::IEcs* ecs) : ecs_(ecs) {}
    ~MockGsComponentManager() override = default;
    // IComponentManager interface
    BASE_NS::string_view GetName() const override { return "MockGsComponentManager"; }
    BASE_NS::Uid GetUid() const override { return GS::IGsComponentManager::UID; }
    size_t GetComponentCount() const override {return 0;}
    const CORE_NS::IPropertyApi& GetPropertyApi() const override {return propertyApi_;}
    CORE_NS::Entity GetEntity(ComponentId index) const override {return CORE_NS::Entity {};}
    uint32_t GetComponentGeneration(ComponentId index) const override {return 0;}
    bool HasComponent(CORE_NS::Entity entity) const override;
    ComponentId GetComponentId(CORE_NS::Entity entity) const override {return 0;}
    void Create(CORE_NS::Entity entity) override;
    bool Destroy(CORE_NS::Entity entity) override;
    void Gc() override {}
    void Destroy(BASE_NS::array_view<const CORE_NS::Entity> gcList) override;
    BASE_NS::vector<CORE_NS::Entity> GetAddedComponents() override { return {}; }
    BASE_NS::vector<CORE_NS::Entity> GetRemovedComponents() override { return {}; }
    BASE_NS::vector<CORE_NS::Entity> GetUpdatedComponents() override { return {}; }
    CORE_NS::ComponentManagerModifiedFlags GetModifiedFlags() const override { return {}; }
    void ClearModifiedFlags() override {}
    uint32_t GetGenerationCounter() const override { return 0; }
    void SetData(CORE_NS::Entity entity, const CORE_NS::IPropertyHandle& data) override {}
    const CORE_NS::IPropertyHandle* GetData(CORE_NS::Entity entity) const override;
    CORE_NS::IPropertyHandle* GetData(CORE_NS::Entity entity) override;
    void SetData(ComponentId index, const CORE_NS::IPropertyHandle& data) override {}
    const CORE_NS::IPropertyHandle* GetData(ComponentId index) const override { return nullptr; }
    CORE_NS::IPropertyHandle* GetData(ComponentId index) override { return nullptr; }
    CORE_NS::IEcs& GetEcs() const override {return *ecs_; }
    BASE_NS::vector<CORE_NS::Entity> GetMovedComponents() override {return {}; }
    // IUriComponentManager generated interface from BEGIN_COMPONENT
    void Set(ComponentId index, const GS::GsComponent& data) override {}
    void Set(CORE_NS::Entity entity, const GS::GsComponent& data) override;
    GS::GsComponent Get(ComponentId index) const override {return GS::GsComponent{};}
    GS::GsComponent Get(CORE_NS::Entity entity) const override;
    CORE_NS::ScopedHandle<const GS::GsComponent> Read(ComponentId index) const override {return CORE_NS::ScopedHandle<const GS::GsComponent>();}
    CORE_NS::ScopedHandle<const GS::GsComponent> Read(CORE_NS::Entity entity) const override;
    CORE_NS::ScopedHandle<GS::GsComponent> Write(ComponentId index) override {return CORE_NS::ScopedHandle<GS::GsComponent>();}
    CORE_NS::ScopedHandle<GS::GsComponent> Write(CORE_NS::Entity entity) override;
private:
    CORE_NS::IEcs* ecs_ = nullptr;
    BASE_NS::unordered_map<uint32_t, GS::GsComponent> components_;
    uint32_t idCounter_ = 0;
    mutable std::mutex mutex_;
    mutable BASE_NS::unordered_map<uint32_t, std::unique_ptr<MockPropertyHandle<GS::GsComponent, GS::IGsComponentManager::UID>>> handles_;
    static MockPropertyApi propertyApi_;
};

// ============================================================================
// MockThreadPool - Mock implementation of IThreadPool
// ============================================================================

class MockThreadPool : public CORE_NS::IThreadPool {
public:
    MockThreadPool() {}
    ~MockThreadPool() override = default;

    const CORE_NS::IInterface* GetInterface(const BASE_NS::Uid& uid) const override { return nullptr; }
    CORE_NS::IInterface* GetInterface(const BASE_NS::Uid& uid) override { return nullptr; }
    void Ref() noexcept override {}
    void Unref() noexcept override {}

    IResult::Ptr Push(ITask::Ptr task) override { return nullptr; }
    IResult::Ptr Push(ITask::Ptr task, BASE_NS::array_view<const ITask* const> dependencies) override { return nullptr; }
    void PushNoWait(ITask::Ptr task) override {}
    void PushNoWait(ITask::Ptr task, BASE_NS::array_view<const ITask* const> dependencies) override {}
    uint32_t GetNumberOfThreads() const override { return 1; }
};

// ============================================================================
// MockECS - Mock implementation of IEcs
// ============================================================================

class MockECS : public CORE_NS::IEcs {
public:
    MockECS();
    ~MockECS() override;

    CORE_NS::IEntityManager& GetEntityManager() override;
    CORE_NS::IComponentManager* GetComponentManager(const BASE_NS::Uid& uid) const override;
    CORE_NS::ISystem* GetSystem(const BASE_NS::Uid& uid) const override;
    void Initialize() override;
    const CORE_NS::IThreadPool::Ptr& GetThreadPool() const override;

    const CORE_NS::IEntityManager& GetEntityManager() const override;
    void GetComponents(CORE_NS::Entity entity, BASE_NS::vector<CORE_NS::IComponentManager*>& result) const override;
    BASE_NS::vector<CORE_NS::ISystem*> GetSystems() const override;
    BASE_NS::vector<CORE_NS::IComponentManager*> GetComponentManagers() const override;
    CORE_NS::Entity CloneEntity(CORE_NS::Entity entity) override;
    void ProcessEvents() override;
    bool Update(uint64_t time, uint64_t delta) override;
    void Uninitialize() override;
    CORE_NS::IComponentManager* CreateComponentManager(const CORE_NS::ComponentManagerTypeInfo& componentManagerTypeInfo) override;
    CORE_NS::ISystem* CreateSystem(const CORE_NS::SystemTypeInfo& systemInfo) override;
    void AddListener(EntityListener& listener) override;
    void RemoveListener(EntityListener& listener) override;
    void AddListener(ComponentListener& listener) override;
    void RemoveListener(ComponentListener& listener) override;
    void AddListener(CORE_NS::IComponentManager& manager, ComponentListener& listener) override;
    void RemoveListener(CORE_NS::IComponentManager& manager, ComponentListener& listener) override;
    void RequestRender() override;
    void SetRenderMode(CORE_NS::IEcs::RenderMode renderMode) override;
    CORE_NS::IEcs::RenderMode GetRenderMode() override;
    bool NeedRender() const override;
    CORE_NS::IClassFactory& GetClassFactory() const override;
    float GetTimeScale() const override;
    void SetTimeScale(float scale) override;
    uint64_t GetId() const override;

    // Mock
    void RegisterManager(CORE_NS::IComponentManager* manager);
    void RegisterSystem(CORE_NS::ISystem* system);
    void SetClassFactory(CORE_NS::IClassFactory* factory);

    void Ref() noexcept override {}
    void Unref() noexcept override {}

private:
    MockEntityManager entityManager_;
    BASE_NS::vector<CORE_NS::IComponentManager*> componentManagers_;
    BASE_NS::vector<CORE_NS::ISystem*> systems_;
    CORE_NS::IThreadPool::Ptr threadPool_;
    CORE_NS::IClassFactory* classFactory_ = nullptr;
    CORE_NS::IEcs::RenderMode renderMode_ = CORE_NS::IEcs::RenderMode::RENDER_IF_DIRTY;
    uint64_t ecsId_ = 0;
};

} // namespace MockFuzz

#endif // MOCKENV_FUZZER_H
