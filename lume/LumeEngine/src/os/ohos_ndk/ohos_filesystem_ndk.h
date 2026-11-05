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

#ifndef CORE_OS_OHOS_NDK_OHOS_FILESYSTEM_NDK_H
#define CORE_OS_OHOS_NDK_OHOS_FILESYSTEM_NDK_H

#include <mutex>

#include <rawfile/raw_file_manager.h>

#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/namespace.h>
#include <core/io/intf_directory.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_system.h>
#include <core/namespace.h>

#include "ohos_file_ndk.h"

CORE_BEGIN_NAMESPACE()

/** Read-only filesystem backed by OHOS NDK rawfile API. */
class OhosNdkFilesystem final : public IFilesystem {
public:
    explicit OhosNdkFilesystem(NativeResourceManager* resMgr);
    OhosNdkFilesystem() = delete;
    OhosNdkFilesystem(OhosNdkFilesystem const&) = delete;
    OhosNdkFilesystem& operator=(OhosNdkFilesystem const&) = delete;
    ~OhosNdkFilesystem() override = default;

    IDirectory::Entry GetEntry(BASE_NS::string_view path) override;
    IFile::Ptr CreateFile(BASE_NS::string_view path) override;
    bool DeleteFile(BASE_NS::string_view path) override;
    bool FileExists(BASE_NS::string_view path) const override;
    IDirectory::Ptr OpenDirectory(BASE_NS::string_view path) override;
    IDirectory::Ptr CreateDirectory(BASE_NS::string_view path) override;
    bool DeleteDirectory(BASE_NS::string_view path) override;
    bool DirectoryExists(BASE_NS::string_view path) const override;

    bool Rename(BASE_NS::string_view fromPath, BASE_NS::string_view toPath) override;
    BASE_NS::vector<BASE_NS::string> GetUriPaths(BASE_NS::string_view uri) const override;
    IFile::Ptr OpenFile(BASE_NS::string_view path, IFile::Mode mode) override;

protected:
    IDirectory::Entry GetEntry(BASE_NS::string_view path) const;
    BASE_NS::string ValidatePath(BASE_NS::string_view pathIn) const;
    void Destroy() override
    {
        delete this;
    }

    BASE_NS::unordered_map<BASE_NS::string, std::weak_ptr<OhosNdkFileStorage>> ndkFiles_;
    BASE_NS::refcnt_ptr<OhosNdkResMgr> resManager_;
    mutable std::mutex mutex_;
};

CORE_END_NAMESPACE()
#endif // CORE_OS_OHOS_NDK_OHOS_FILESYSTEM_NDK_H
