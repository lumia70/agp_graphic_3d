/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#include "ohos_file_ndk.h"

#include <cstdint>
#include <cstring>

#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <core/io/intf_file.h>
#include <core/log.h>
#include <core/namespace.h>

CORE_BEGIN_NAMESPACE()
using BASE_NS::CloneData;
using BASE_NS::string;
using BASE_NS::string_view;

// --- OhosNdkFileDirectory ---

OhosNdkFileDirectory::OhosNdkFileDirectory(BASE_NS::refcnt_ptr<OhosNdkResMgr> resMgr) : dirResMgr_(resMgr) {}

OhosNdkFileDirectory::~OhosNdkFileDirectory()
{
    Close();
}

void OhosNdkFileDirectory::Close()
{
    fileList_.clear();
    dirPath_.clear();
}

bool OhosNdkFileDirectory::IsDir(BASE_NS::string_view path) const
{
    NativeResourceManager* mgr = dirResMgr_->GetResMgr();
    if (!mgr) {
        return false;
    }
    RawDir* rawDir = OH_ResourceManager_OpenRawDir(mgr, path.data());
    if (!rawDir) {
        return false;
    }
    int count = OH_ResourceManager_GetRawFileCount(rawDir);
    OH_ResourceManager_CloseRawDir(rawDir);
    return (count > 0);
}

bool OhosNdkFileDirectory::IsFile(BASE_NS::string_view path) const
{
    NativeResourceManager* mgr = dirResMgr_->GetResMgr();
    if (!mgr) {
        return false;
    }
    RawFile* rawFile = OH_ResourceManager_OpenRawFile(mgr, path.data());
    if (!rawFile) {
        return false;
    }
    OH_ResourceManager_CloseRawFile(rawFile);
    return true;
}

bool OhosNdkFileDirectory::Open(const BASE_NS::string_view pathIn)
{
    if (pathIn.empty()) {
        return false;
    }
    auto path = pathIn;
    if (path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }

    NativeResourceManager* mgr = dirResMgr_->GetResMgr();
    if (!mgr) {
        CORE_LOG_E("OhosNdkFileDirectory::Open: NativeResourceManager is null");
        return false;
    }

    RawDir* rawDir = OH_ResourceManager_OpenRawDir(mgr, string(path).c_str());
    if (!rawDir) {
        CORE_LOG_E("OhosNdkFileDirectory::Open: OH_ResourceManager_OpenRawDir failed for '%s'",
            string(path).c_str());
        return false;
    }

    int count = OH_ResourceManager_GetRawFileCount(rawDir);
    fileList_.clear();
    fileList_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const char* name = OH_ResourceManager_GetRawFileName(rawDir, i);
        if (name) {
            fileList_.push_back(string(name));
        }
    }
    OH_ResourceManager_CloseRawDir(rawDir);

    dirPath_ = string(path);
    return true;
}

BASE_NS::vector<IDirectory::Entry> OhosNdkFileDirectory::GetEntries() const
{
    BASE_NS::vector<IDirectory::Entry> result;
    for (size_t i = 0; i < fileList_.size(); i++) {
        auto fullPath = dirPath_ + "/" + fileList_[i];
        auto entry = GetEntry(fullPath);
        entry.timestamp = static_cast<uint64_t>(i);
        entry.name = fileList_[i];
        result.emplace_back(BASE_NS::move(entry));
    }
    return result;
}

IDirectory::Entry OhosNdkFileDirectory::GetEntry(BASE_NS::string_view uriIn) const
{
    if (!uriIn.empty()) {
        IDirectory::Entry::Type type;
        if (IsFile(uriIn)) {
            type = IDirectory::Entry::FILE;
        } else if (IsDir(uriIn)) {
            type = IDirectory::Entry::DIRECTORY;
        } else {
            type = IDirectory::Entry::UNKNOWN;
        }
        uint64_t timestamp = 0;
        string entryName { uriIn };
        return IDirectory::Entry { type, entryName, timestamp };
    }
    return {};
}

// --- OhosNdkFile ---

OhosNdkFile::OhosNdkFile(BASE_NS::refcnt_ptr<OhosNdkResMgr> resMgr) : fileResMgr_(resMgr)
{
    buffer_ = std::make_shared<OhosNdkFileStorage>(nullptr);
}

void OhosNdkFile::UpdateStorage(std::shared_ptr<OhosNdkFileStorage> buffer)
{
    buffer_ = std::move(buffer);
}

IFile::Mode OhosNdkFile::GetMode() const
{
    return IFile::Mode::READ_ONLY;
}

void OhosNdkFile::Close() {}

uint64_t OhosNdkFile::Read(void* buffer, uint64_t count)
{
    uint64_t toRead = count;
    uint64_t sum = index_ + toRead;
    if (sum < index_) {
        // overflow
        return 0;
    }
    if (sum > buffer_->Size()) {
        toRead = buffer_->Size() - index_;
    }
    if (toRead <= 0) {
        return toRead;
    }
    if (toRead > SIZE_MAX) {
        CORE_LOG_E("OhosNdkFile::Read: Unable to read chunks bigger than SIZE_MAX bytes.");
        return 0;
    }
    if (CloneData(buffer, static_cast<size_t>(count), &(buffer_->GetStorage()[index_]), static_cast<size_t>(toRead))) {
        index_ += toRead;
    }
    return toRead;
}

uint64_t OhosNdkFile::Write(const void* buffer, uint64_t count)
{
    // read-only
    return 0;
}

uint64_t OhosNdkFile::Append(const void* buffer, uint64_t count, uint64_t flushSize)
{
    // read-only
    return 0;
}

uint64_t OhosNdkFile::GetLength() const
{
    return buffer_->Size();
}

bool OhosNdkFile::Seek(uint64_t aOffset)
{
    if (aOffset < buffer_->Size()) {
        index_ = aOffset;
        return true;
    }
    return false;
}

uint64_t OhosNdkFile::GetPosition() const
{
    return index_;
}

std::shared_ptr<OhosNdkFileStorage> OhosNdkFile::Open(BASE_NS::string_view path)
{
    NativeResourceManager* mgr = fileResMgr_->GetResMgr();
    if (!mgr) {
        CORE_LOG_E("OhosNdkFile::Open: NativeResourceManager is null");
        return nullptr;
    }

    RawFile* rawFile = OH_ResourceManager_OpenRawFile(mgr, path.data());
    if (!rawFile) {
        CORE_LOG_E("OhosNdkFile::Open: OH_ResourceManager_OpenRawFile failed for '%s'", path.data());
        return nullptr;
    }

    long fileSize = OH_ResourceManager_GetRawFileSize(rawFile);
    if (fileSize <= 0) {
        OH_ResourceManager_CloseRawFile(rawFile);
        CORE_LOG_E("OhosNdkFile::Open: raw file size is %ld for '%s'", fileSize, path.data());
        return nullptr;
    }

    auto dataLen = static_cast<size_t>(fileSize);
    auto data = std::make_unique<uint8_t[]>(dataLen);

    int readResult = OH_ResourceManager_ReadRawFile(rawFile, data.get(), dataLen);
    OH_ResourceManager_CloseRawFile(rawFile);

    if (readResult < 0) {
        CORE_LOG_E("OhosNdkFile::Open: OH_ResourceManager_ReadRawFile failed for '%s', result: %d",
            path.data(), readResult);
        return nullptr;
    }

    buffer_->SetBuffer(std::move(data), dataLen);
    return buffer_;
}

CORE_END_NAMESPACE()
