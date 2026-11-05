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

#include "mockenv_fuzzer.h"

#include <fstream>
#include <algorithm>
#include <cstring>

namespace MockFuzz {

// ============================================================================
// MockFile Implementation (移植自 MemoryFile)
// ============================================================================

CORE_NS::IFile::Mode MockFile::GetMode() const
{
    return Mode::READ_WRITE;
}
void MockFile::Close()
{
    pos_ = 0;
}
uint64_t MockFile::Read(void* buffer, uint64_t count)
{
    if (buffer == nullptr || count == 0) {
        return 0;
    }
    
    size_t readCount = static_cast<size_t>(count);
    if (pos_ + readCount > size_) {
        readCount = size_ - pos_;
    }
    
    std::memcpy(buffer, buf_ + pos_, readCount);
    pos_ += readCount;
    return static_cast<uint64_t>(readCount);
}
uint64_t MockFile::Write(const void* buffer, uint64_t count)
{
    if (buffer == nullptr || count == 0) {
        return 0;
    }
    size_t need = pos_ + static_cast<size_t>(count);
    if (need < pos_) {
        return 0;
    }
    if (need > cap_) {
        // Allocate exactly needed bytes for ASan redzone.
        void* newBuf = ::malloc(need);
        if (!newBuf) {
            return 0;
        }
        if (buf_ != nullptr && size_ > 0) {
            std::memcpy(newBuf, buf_, size_);
        }
        ::free(buf_);
        buf_ = static_cast<uint8_t*>(newBuf);
        cap_ = need;
    }
    std::memcpy(buf_ + pos_, buffer, static_cast<size_t>(count));
    pos_ += static_cast<size_t>(count);
    if (pos_ > size_) {
        size_ = pos_;
    }
    return count;
}
uint64_t MockFile::Append(const void* buffer, uint64_t count, uint64_t flushSize)
{
    pos_ = size_;
    return Write(buffer, count);
}
uint64_t MockFile::GetLength() const
{
    return size_;
}
bool MockFile::Seek(uint64_t offset)
{
    if (offset > size_) {
        return false;
    }
    pos_ = static_cast<size_t>(offset);
    return true;
}
uint64_t MockFile::GetPosition() const
{
    return pos_;
}
void MockFile::Destroy()
{
    // no-op; MockFileManager::Clear() handles cleanup
}

// ============================================================================
// MockFileManager Implementation (移植自 FuzzFileManager)
// ============================================================================

MockFileManager::MockFileManager() {}
MockFileManager::~MockFileManager()
{
    Clear();
}
void MockFileManager::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* f : allFiles_) {
        delete f;
    }
    allFiles_.clear();
    files_.clear();
}
const CORE_NS::IInterface* MockFileManager::GetInterface(const BASE_NS::Uid& uid) const
{
    if (uid == CORE_NS::IFileManager::UID || uid == CORE_NS::IInterface::UID) {
        return this;
    }
    return nullptr;
}
CORE_NS::IInterface* MockFileManager::GetInterface(const BASE_NS::Uid& uid)
{
    if (uid == CORE_NS::IFileManager::UID || uid == CORE_NS::IInterface::UID) {
        return this;
    }
    return nullptr;
}
void MockFileManager::Ref() noexcept {}
void MockFileManager::Unref() noexcept {}
CORE_NS::IFile::Ptr MockFileManager::CreateFile(BASE_NS::string_view uri)
{
    auto* file = new MockFile();
    std::lock_guard<std::mutex> lock(mutex_);
    files_[BASE_NS::string(uri)] = file;
    allFiles_.push_back(file);
    return CORE_NS::IFile::Ptr(file);
}
CORE_NS::IFile::Ptr MockFileManager::OpenFile(BASE_NS::string_view uri)
{
    return OpenFile(uri, CORE_NS::IFile::Mode::READ_ONLY);
}
CORE_NS::IFile::Ptr MockFileManager::OpenFile(BASE_NS::string_view uri, CORE_NS::IFile::Mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(BASE_NS::string(uri));
    if (it != files_.end()) {
        it->second->Seek(0);
        return CORE_NS::IFile::Ptr(it->second);
    }
    return CORE_NS::IFile::Ptr();
}
bool MockFileManager::DeleteFile(BASE_NS::string_view uri)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = files_.find(BASE_NS::string(uri));
    if (it != files_.end()) {
        files_.erase(it); // lookup-only removal, allFiles_ still owns it
        return true;
    }
    return false;
}
bool MockFileManager::FileExists(BASE_NS::string_view uri) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.find(BASE_NS::string(uri)) != files_.end();
}
CORE_NS::IDirectory::Entry MockFileManager::GetEntry(BASE_NS::string_view uri)
{
    return { CORE_NS::IDirectory::Entry::UNKNOWN, "", 0 };
}
CORE_NS::IDirectory::Ptr MockFileManager::OpenDirectory(BASE_NS::string_view uri)
{
    return {};
}
CORE_NS::IDirectory::Ptr MockFileManager::CreateDirectory(BASE_NS::string_view uri)
{
    return {};
}
bool MockFileManager::DeleteDirectory(BASE_NS::string_view uri)
{
    return false;
}
bool MockFileManager::DirectoryExists(BASE_NS::string_view uri) const
{
    return false;
}
bool MockFileManager::Rename(BASE_NS::string_view fromUri, BASE_NS::string_view toUri)
{
    return false;
}
bool MockFileManager::RegisterFilesystem(BASE_NS::string_view protocol, CORE_NS::IFilesystem::Ptr filesystem)
{
    return false;
}
void MockFileManager::UnregisterFilesystem(BASE_NS::string_view protocol) {}
void MockFileManager::RegisterAssetPath(BASE_NS::string_view uri) {}
void MockFileManager::UnregisterAssetPath(BASE_NS::string_view uri) {}
bool MockFileManager::CheckExistence(BASE_NS::string_view protocol)
{
    return false;
}
bool MockFileManager::RegisterPath(BASE_NS::string_view protocol, BASE_NS::string_view uri, bool prepend)
{
    return false;
}
void MockFileManager::UnregisterPath(BASE_NS::string_view protocol, BASE_NS::string_view uri) {}
CORE_NS::IFilesystem::Ptr MockFileManager::CreateROFilesystem(const void* const data, uint64_t size)
{
    return {};
}
CORE_NS::IFilesystem* MockFileManager::GetFilesystem(BASE_NS::string_view protocol) const
{
    return nullptr;
}

// ============================================================================
// MockEntityManager Implementation
// ============================================================================

MockEntityManager::MockEntityManager() {}
CORE_NS::EntityReference MockEntityManager::CreateReferenceCounted()
{
    CORE_NS::Entity entity;
    entity.id = idCounter_++;
    entities_.push_back(entity);
    
    CORE_NS::EntityReference ref;
    ref.entity_ = entity;
    return ref;
}
CORE_NS::Entity MockEntityManager::Create()
{
    return {};
}
CORE_NS::EntityReference MockEntityManager::GetReferenceCounted(CORE_NS::Entity entity)
{
    return {};
}
void MockEntityManager::Destroy(const CORE_NS::Entity entity) {}
void MockEntityManager::DestroyAllEntities() {}
bool MockEntityManager::IsAlive(const CORE_NS::Entity entity) const
{
    return entity.id != CORE_NS::INVALID_ENTITY && entity.id != 0;
}
void MockEntityManager::SetActive(const CORE_NS::Entity entity, bool state) {}
uint32_t MockEntityManager::GetGenerationCounter() const
{
    return static_cast<uint32_t>(idCounter_);
}
CORE_NS::IEntityManager::Iterator::Ptr MockEntityManager::Begin(IteratorType type) const
{
    return nullptr;
}
CORE_NS::IEntityManager::Iterator::Ptr MockEntityManager::End(IteratorType type) const
{
    return nullptr;
}

// ============================================================================
// MockSceneNode Implementation
// ============================================================================

MockSceneNode::MockSceneNode(CORE_NS::Entity entity) : entity_(entity) {}
CORE_NS::Entity MockSceneNode::GetEntity() const
{
    return entity_;
}
void MockSceneNode::SetParent(CORE3D_NS::ISceneNode const& node)
{
    parent_ = &node;
}
CORE3D_NS::ISceneNode* MockSceneNode::GetParent() const
{
    return const_cast<CORE3D_NS::ISceneNode*>(parent_);
}
BASE_NS::string MockSceneNode::GetName() const
{
    return "MockNode";
}
void MockSceneNode::SetName(BASE_NS::string_view name) {}
void MockSceneNode::SetEnabled(bool isEnabled) {}
bool MockSceneNode::GetEnabled() const
{
    return true;
}
bool MockSceneNode::GetEffectivelyEnabled() const
{
    return true;
}
bool MockSceneNode::IsAncestorOf(CORE3D_NS::ISceneNode const& node)
{
    return false;
}
BASE_NS::array_view<CORE3D_NS::ISceneNode* const> MockSceneNode::GetChildren() const
{
    return {};
}
BASE_NS::array_view<CORE3D_NS::ISceneNode*> MockSceneNode::GetChildren()
{
    return {};
}
const CORE3D_NS::ISceneNode* MockSceneNode::GetChild(BASE_NS::string_view const& name) const
{
    return nullptr;
}
CORE3D_NS::ISceneNode* MockSceneNode::GetChild(BASE_NS::string_view const& name)
{
    return nullptr;
}
bool MockSceneNode::AddChild(CORE3D_NS::ISceneNode& node)
{
    return false;
}
bool MockSceneNode::InsertChild(size_t index, CORE3D_NS::ISceneNode& node)
{
    return false;
}
bool MockSceneNode::RemoveChild(CORE3D_NS::ISceneNode& node)
{
    return false;
}
bool MockSceneNode::RemoveChild(size_t index)
{
    return false;
}
bool MockSceneNode::RemoveChildren()
{
    return false;
}
const CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByPath(BASE_NS::string_view const& path) const
{
    return nullptr;
}
CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByPath(BASE_NS::string_view const& path)
{
    return nullptr;
}
const CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByName(BASE_NS::string_view const& name) const
{
    return nullptr;
}
CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByName(BASE_NS::string_view const& name)
{
    return nullptr;
}
const CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByComponent(const CORE_NS::IComponentManager& componentManager) const
{
    return nullptr;
}
CORE3D_NS::ISceneNode* MockSceneNode::LookupNodeByComponent(const CORE_NS::IComponentManager& componentManager)
{
    return nullptr;
}
BASE_NS::vector<const CORE3D_NS::ISceneNode*> MockSceneNode::LookupNodesByComponent(const CORE_NS::IComponentManager& componentManager) const
{
    return {};
}
BASE_NS::vector<CORE3D_NS::ISceneNode*> MockSceneNode::LookupNodesByComponent(const CORE_NS::IComponentManager& componentManager)
{
    return {};
}
BASE_NS::Math::Vec3 MockSceneNode::GetPosition() const
{
    return BASE_NS::Math::Vec3 { 0.0f, 0.0f, 0.0f };
}
BASE_NS::Math::Quat MockSceneNode::GetRotation() const
{
    return BASE_NS::Math::Quat { 0.0f, 0.0f, 0.0f, 1.0f };
}
BASE_NS::Math::Vec3 MockSceneNode::GetScale() const
{
    return BASE_NS::Math::Vec3 { 1.0f, 1.0f, 1.0f };
}
void MockSceneNode::SetScale(const BASE_NS::Math::Vec3& scale) {}
void MockSceneNode::SetPosition(const BASE_NS::Math::Vec3& position) {}
void MockSceneNode::SetRotation(const BASE_NS::Math::Quat& rotation) {}
uint32_t MockSceneNode::GetSceneId() const
{
    return sceneId_;
}
void MockSceneNode::SetSceneId(uint32_t sceneId)
{
    sceneId_ = sceneId;
}

// ============================================================================
// MockNodeSystem Implementation
// ============================================================================

MockNodeSystem::MockNodeSystem(CORE_NS::IEcs* ecs) : ecs_(ecs)
{
    rootNode_ = std::make_unique<MockSceneNode>(CORE_NS::Entity {});
}
CORE3D_NS::ISceneNode* MockNodeSystem::CreateNode()
{
    CORE_NS::Entity entity;
    entity.id = static_cast<uint32_t>(nodes_.size()) + 1;
    
    std::unique_ptr<CORE3D_NS::ISceneNode> node = std::make_unique<MockSceneNode>(entity);
    nodes_.push_back(std::move(node));
    return nodes_.back().get();
}
CORE3D_NS::ISceneNode* MockNodeSystem::GetNode(CORE_NS::Entity entity) const
{
    for (const auto& node : nodes_) {
        if (node->GetEntity() == entity) {
            return node.get();
        }
    }
    return nullptr;
}
CORE3D_NS::ISceneNode& MockNodeSystem::GetRootNode() const
{
    return *rootNode_;
}
CORE3D_NS::ISceneNode* MockNodeSystem::CloneNode(const CORE3D_NS::ISceneNode& node, bool recursive)
{
    return nullptr;
}
void MockNodeSystem::DestroyNode(CORE3D_NS::ISceneNode& node)
{
    auto it = std::remove_if(nodes_.begin(), nodes_.end(),
        [&node](const std::unique_ptr<CORE3D_NS::ISceneNode>& ptr) {
            return ptr.get() == &node;
        });
    nodes_.erase(it, nodes_.end());
}

void MockNodeSystem::RefreshAllNodes() {}
void MockNodeSystem::AddListener(SceneNodeListener& listener) {}
void MockNodeSystem::RemoveListener(SceneNodeListener& listener) {}

// ============================================================================
// MockUriComponentManager Implementation
// ============================================================================
bool MockUriComponentManager::HasComponent(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return components_.find(entity.id) != components_.end();
}
void MockUriComponentManager::Create(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    components_[entity.id] = CORE3D_NS::UriComponent{};
}
bool MockUriComponentManager::Destroy(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    handles_.erase(entity.id);
    return components_.erase(entity.id) > 0;
}
void MockUriComponentManager::Destroy(BASE_NS::array_view<const CORE_NS::Entity> gcList)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entity : gcList) {
        components_.erase(entity.id);
        handles_.erase(entity.id);
    }
}
const CORE_NS::IPropertyHandle* MockUriComponentManager::GetData(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        // Create or return cached handle
        auto handleIt = handles_.find(entity.id);
        if (handleIt == handles_.end()) {
            auto handle = std::make_unique<MockPropertyHandle<CORE3D_NS::UriComponent, CORE3D_NS::IUriComponentManager::UID>>(const_cast<CORE3D_NS::UriComponent*>(&it->second));
            handleIt = handles_.insert({entity.id, std::move(handle)}).first;
        }
        return static_cast<const CORE_NS::IPropertyHandle*>(handleIt->second.get());
    }
    return nullptr;
}
CORE_NS::IPropertyHandle* MockUriComponentManager::GetData(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        // Create or return cached handle
        auto handleIt = handles_.find(entity.id);
        if (handleIt == handles_.end()) {
            auto handle = std::make_unique<MockPropertyHandle<CORE3D_NS::UriComponent, CORE3D_NS::IUriComponentManager::UID>>(&it->second);
            handleIt = handles_.insert({entity.id, std::move(handle)}).first;
        }
        return static_cast<CORE_NS::IPropertyHandle*>(handleIt->second.get());
    }
    return nullptr;
}
void MockUriComponentManager::Set(CORE_NS::Entity entity, const CORE3D_NS::UriComponent& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    components_[entity.id] = data;
}
CORE3D_NS::UriComponent MockUriComponentManager::Get(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        return it->second;
    }
    return CORE3D_NS::UriComponent{};
}
CORE_NS::ScopedHandle<const CORE3D_NS::UriComponent> MockUriComponentManager::Read(CORE_NS::Entity entity) const
{
    const CORE_NS::IPropertyHandle* handle = GetData(entity);
    return CORE_NS::ScopedHandle<const CORE3D_NS::UriComponent>(handle);
}
CORE_NS::ScopedHandle<CORE3D_NS::UriComponent> MockUriComponentManager::Write(CORE_NS::Entity entity)
{
    CORE_NS::IPropertyHandle* handle = GetData(entity);
    return CORE_NS::ScopedHandle<CORE3D_NS::UriComponent>(handle);
}

// ============================================================================
// MockGsComponentManager Implementation
// ============================================================================
bool MockGsComponentManager::HasComponent(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return components_.find(entity.id) != components_.end();
}
void MockGsComponentManager::Create(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    components_[entity.id] = GS::GsComponent{};
}
bool MockGsComponentManager::Destroy(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    handles_.erase(entity.id);
    return components_.erase(entity.id) > 0;
}
void MockGsComponentManager::Destroy(BASE_NS::array_view<const CORE_NS::Entity> gcList)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entity : gcList) {
        components_.erase(entity.id);
        handles_.erase(entity.id);
    }
}
const CORE_NS::IPropertyHandle* MockGsComponentManager::GetData(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        // Create or return cached handle
        auto handleIt = handles_.find(entity.id);
        if (handleIt == handles_.end()) {
            auto handle = std::make_unique<MockPropertyHandle<GS::GsComponent, GS::IGsComponentManager::UID>>(const_cast<GS::GsComponent*>(&it->second));
            handleIt = handles_.insert({entity.id, std::move(handle)}).first;
        }
        return static_cast<const CORE_NS::IPropertyHandle*>(handleIt->second.get());
    }
    return nullptr;
}
CORE_NS::IPropertyHandle* MockGsComponentManager::GetData(CORE_NS::Entity entity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        // Create or return cached handle
        auto handleIt = handles_.find(entity.id);
        if (handleIt == handles_.end()) {
            auto handle = std::make_unique<MockPropertyHandle<GS::GsComponent, GS::IGsComponentManager::UID>>(&it->second);
            handleIt = handles_.insert({entity.id, std::move(handle)}).first;
        }
        return static_cast<CORE_NS::IPropertyHandle*>(handleIt->second.get());
    }
    return nullptr;
}
void MockGsComponentManager::Set(CORE_NS::Entity entity, const GS::GsComponent& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    components_[entity.id] = data;
}
GS::GsComponent MockGsComponentManager::Get(CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = components_.find(entity.id);
    if (it != components_.end()) {
        return it->second;
    }
    return GS::GsComponent{};
}
CORE_NS::ScopedHandle<const GS::GsComponent> MockGsComponentManager::Read(CORE_NS::Entity entity) const
{
    const CORE_NS::IPropertyHandle* handle = GetData(entity);
    return CORE_NS::ScopedHandle<const GS::GsComponent>(handle);
}
CORE_NS::ScopedHandle<GS::GsComponent> MockGsComponentManager::Write(CORE_NS::Entity entity)
{
    CORE_NS::IPropertyHandle* handle = GetData(entity);
    return CORE_NS::ScopedHandle<GS::GsComponent>(handle);
}

MockPropertyApi MockUriComponentManager::propertyApi_;
MockPropertyApi MockGsComponentManager::propertyApi_;

// ============================================================================
// MockECS Implementation
// ============================================================================

MockECS::MockECS() : threadPool_(CORE_NS::IThreadPool::Ptr(new MockThreadPool())) {}
MockECS::~MockECS()
{
    for (auto* manager : componentManagers_) {
        if (manager) {
            delete manager;
        }
    }
    componentManagers_.clear();
    
    for (auto* system : systems_) {
        if (system) {
            delete system;
        }
    }
    systems_.clear();
}
CORE_NS::IEntityManager& MockECS::GetEntityManager()
{
    return entityManager_;
}
const CORE_NS::IEntityManager& MockECS::GetEntityManager() const
{
    return entityManager_;
}
CORE_NS::IComponentManager* MockECS::GetComponentManager(const BASE_NS::Uid& uid) const
{
    for (auto* manager : componentManagers_) {
        if (manager->GetUid() == uid) {
            return manager;
        }
    }
    return nullptr;
}
CORE_NS::ISystem* MockECS::GetSystem(const BASE_NS::Uid& uid) const
{
    for (auto* system : systems_) {
        if (system->GetUid() == uid) {
            return system;
        }
    }
    return nullptr;
}
void MockECS::GetComponents(CORE_NS::Entity entity, BASE_NS::vector<CORE_NS::IComponentManager*>& result) const {}
BASE_NS::vector<CORE_NS::ISystem*> MockECS::GetSystems() const
{
    return systems_;
}
BASE_NS::vector<CORE_NS::IComponentManager*> MockECS::GetComponentManagers() const
{
    return componentManagers_;
}
CORE_NS::Entity MockECS::CloneEntity(CORE_NS::Entity entity)
{
    return {};
}
void MockECS::ProcessEvents() {}
void MockECS::Initialize() {}
bool MockECS::Update(uint64_t time, uint64_t delta)
{
    return false;
}
void MockECS::Uninitialize() {}
CORE_NS::IComponentManager* MockECS::CreateComponentManager(const CORE_NS::ComponentManagerTypeInfo& componentManagerTypeInfo)
{
    return nullptr;
}
CORE_NS::ISystem* MockECS::CreateSystem(const CORE_NS::SystemTypeInfo& systemInfo)
{
    return nullptr;
}
void MockECS::AddListener(EntityListener& listener) {}
void MockECS::RemoveListener(EntityListener& listener) {}
void MockECS::AddListener(ComponentListener& listener) {}
void MockECS::RemoveListener(ComponentListener& listener) {}
void MockECS::AddListener(CORE_NS::IComponentManager& manager, ComponentListener& listener) {}
void MockECS::RemoveListener(CORE_NS::IComponentManager& manager, ComponentListener& listener) {}
void MockECS::RequestRender() {}
void MockECS::SetRenderMode(CORE_NS::IEcs::RenderMode renderMode)
{
    renderMode_ = renderMode;
}
CORE_NS::IEcs::RenderMode MockECS::GetRenderMode()
{
    return renderMode_;
}
bool MockECS::NeedRender() const
{
    return false;
}
CORE_NS::IClassFactory& MockECS::GetClassFactory() const
{
    return *classFactory_;
}
const CORE_NS::IThreadPool::Ptr& MockECS::GetThreadPool() const
{
    return threadPool_;
}
float MockECS::GetTimeScale() const
{
    return 1.0f;
}
void MockECS::SetTimeScale(float scale) {}
uint64_t MockECS::GetId() const
{
    return ecsId_;
}
void MockECS::RegisterManager(CORE_NS::IComponentManager* manager)
{
    if (manager) {
        componentManagers_.push_back(manager);
    }
}
void MockECS::RegisterSystem(CORE_NS::ISystem* system)
{
    if (system) {
        systems_.push_back(system);
    }
}
void MockECS::SetClassFactory(CORE_NS::IClassFactory* factory)
{
    classFactory_ = factory;
}

} // namespace MockFuzz
