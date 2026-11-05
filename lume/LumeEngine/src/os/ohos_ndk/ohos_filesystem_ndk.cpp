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

#include "ohos_filesystem_ndk.h"

#include <algorithm>
#include <cstring>

#include <base/containers/iterator.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/type_traits.h>
#include <base/containers/unique_ptr.h>
#include <base/containers/vector.h>
#include <base/namespace.h>
#include <core/io/intf_directory.h>
#include <core/io/intf_file.h>
#include <core/log.h>
#include <core/namespace.h>

#include "io/path_tools.h"

CORE_BEGIN_NAMESPACE()
using BASE_NS::move;
using BASE_NS::string;
using BASE_NS::string_view;
using BASE_NS::vector;

BASE_NS::string OhosNdkFilesystem::ValidatePath(const string_view pathIn) const
{
    auto path = NormalizePath(pathIn);
    if (!path.empty() && (path.back() == '/')) {
        size_t len = path.length();
        path.resize(len - 1U);
    }
    // Remove leading '/', not used in rawfile API paths.
    if (!path.empty() && path.front() == '/') {
        path.erase(path.cbegin());
    }
    return path;
}

OhosNdkFilesystem::OhosNdkFilesystem(NativeResourceManager* resMgr)
{
    resManager_ = BASE_NS::refcnt_ptr<OhosNdkResMgr>(new OhosNdkResMgr(resMgr));
}

IFile::Ptr OhosNdkFilesystem::OpenFile(const BASE_NS::string_view path, const IFile::Mode mode)
{
    if (mode == IFile::Mode::READ_ONLY) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto const pos = ndkFiles_.find(path); pos != ndkFiles_.end()) {
            auto storage = pos->second.lock();
            if (storage) {
                auto file = BASE_NS::make_unique<OhosNdkFile>(resManager_);
                file->UpdateStorage(storage);
                return IFile::Ptr { file.release() };
            }
        }
        if (!path.empty()) {
            auto file = BASE_NS::make_unique<OhosNdkFile>(resManager_);
            auto storage = file->Open(path.data());
            if (storage) {
                ndkFiles_[path] = storage;
                return IFile::Ptr { file.release() };
            }
        }
    }
    return IFile::Ptr();
}

IDirectory::Entry OhosNdkFilesystem::GetEntry(BASE_NS::string_view path) const
{
    if (!path.empty()) {
        auto directory = BASE_NS::make_unique<OhosNdkFileDirectory>(resManager_);
        return directory->GetEntry(path.data());
    }
    return {};
}

IDirectory::Entry OhosNdkFilesystem::GetEntry(BASE_NS::string_view path)
{
    const auto& self = *this;
    return self.GetEntry(path);
}

IFile::Ptr OhosNdkFilesystem::CreateFile(BASE_NS::string_view path)
{
    // read-only filesystem
    return IFile::Ptr();
}

bool OhosNdkFilesystem::DeleteFile(BASE_NS::string_view path)
{
    // read-only filesystem
    return false;
}

bool OhosNdkFilesystem::FileExists(const string_view path) const
{
    return GetEntry(path).type == IDirectory::Entry::Type::FILE;
}

IDirectory::Ptr OhosNdkFilesystem::OpenDirectory(BASE_NS::string_view pathIn)
{
    auto path = ValidatePath(pathIn);
    auto directory = BASE_NS::make_unique<OhosNdkFileDirectory>(resManager_);
    if (directory->Open(path.data())) {
        return IDirectory::Ptr { directory.release() };
    }
    return IDirectory::Ptr();
}

IDirectory::Ptr OhosNdkFilesystem::CreateDirectory(BASE_NS::string_view path)
{
    // read-only filesystem
    return IDirectory::Ptr();
}

bool OhosNdkFilesystem::DeleteDirectory(BASE_NS::string_view path)
{
    // read-only filesystem
    return false;
}

bool OhosNdkFilesystem::DirectoryExists(const string_view path) const
{
    return GetEntry(path).type == IDirectory::Entry::Type::DIRECTORY;
}

bool OhosNdkFilesystem::Rename(BASE_NS::string_view fromPath, BASE_NS::string_view toPath)
{
    // read-only filesystem
    return false;
}

BASE_NS::vector<BASE_NS::string> OhosNdkFilesystem::GetUriPaths(BASE_NS::string_view uri) const
{
    return {};
}

CORE_END_NAMESPACE()
