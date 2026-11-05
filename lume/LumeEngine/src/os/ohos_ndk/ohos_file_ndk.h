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

#ifndef CORE_OS_OHOS_NDK_OHOS_FILE_NDK_H
#define CORE_OS_OHOS_NDK_OHOS_FILE_NDK_H

#include <atomic>
#include <memory>
#include <mutex>

#include <rawfile/raw_dir.h>
#include <rawfile/raw_file.h>
#include <rawfile/raw_file_manager.h>

#include <base/containers/refcnt_ptr.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/unique_ptr.h>
#include <base/containers/vector.h>
#include <core/io/intf_directory.h>
#include <core/io/intf_file.h>
#include <core/namespace.h>

CORE_BEGIN_NAMESPACE()

/** Wrapper around NativeResourceManager* for reference-counted sharing.
 *  Does NOT own the NativeResourceManager; the caller is responsible for its lifetime. */
class OhosNdkResMgr {
public:
    explicit OhosNdkResMgr(NativeResourceManager* resMgr) : resMgr_(resMgr) {}

    NativeResourceManager* GetResMgr() const
    {
        return resMgr_;
    }

    void Ref()
    {
        refcnt_.fetch_add(1, std::memory_order_relaxed);
    }

    void Unref()
    {
        if (refcnt_.fetch_sub(1, std::memory_order_release) == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete this;
        }
    }

private:
    std::atomic_int32_t refcnt_ { 0 };
    NativeResourceManager* resMgr_ { nullptr };
};

/** Stores the raw file data read into memory. */
class OhosNdkFileStorage {
public:
    explicit OhosNdkFileStorage(std::unique_ptr<uint8_t[]> buffer) : buffer_(std::move(buffer)) {}
    ~OhosNdkFileStorage() = default;

    const std::unique_ptr<uint8_t[]>& GetStorage() const
    {
        return buffer_;
    }

    size_t Size() const
    {
        return size_;
    }

    void SetBuffer(std::unique_ptr<uint8_t[]> buffer, size_t size)
    {
        buffer_ = std::move(buffer);
        size_ = size;
    }

private:
    std::unique_ptr<uint8_t[]> buffer_;
    size_t size_ { 0 };
};

/** Directory implementation backed by NDK RawDir API. */
class OhosNdkFileDirectory final : public IDirectory {
public:
    explicit OhosNdkFileDirectory(BASE_NS::refcnt_ptr<OhosNdkResMgr> resMgr);
    ~OhosNdkFileDirectory() override;

    bool Open(BASE_NS::string_view path);
    void Close() override;
    BASE_NS::vector<Entry> GetEntries() const override;

    IDirectory::Entry GetEntry(BASE_NS::string_view uriIn) const;
    bool IsFile(BASE_NS::string_view path) const;
    bool IsDir(BASE_NS::string_view path) const;

protected:
    void Destroy() override
    {
        delete this;
    }

private:
    BASE_NS::string dirPath_;
    BASE_NS::vector<BASE_NS::string> fileList_;
    BASE_NS::refcnt_ptr<OhosNdkResMgr> dirResMgr_;
};

/** IFile implementation that reads entire raw file into memory via NDK RawFile API. */
class OhosNdkFile final : public IFile {
public:
    explicit OhosNdkFile(BASE_NS::refcnt_ptr<OhosNdkResMgr> resMgr);
    ~OhosNdkFile() override = default;

    Mode GetMode() const override;
    void Close() override;
    uint64_t Read(void* buffer, uint64_t count) override;
    uint64_t Write(const void* buffer, uint64_t count) override;
    uint64_t Append(const void* buffer, uint64_t count, uint64_t flushSize) override;
    uint64_t GetLength() const override;
    bool Seek(uint64_t offset) override;
    uint64_t GetPosition() const override;

    /** Open raw file by path, reading all contents into memory.
     *  @return shared storage on success, nullptr on failure. */
    std::shared_ptr<OhosNdkFileStorage> Open(BASE_NS::string_view path);

    /** Replace internal storage (e.g. for cache reuse). */
    void UpdateStorage(std::shared_ptr<OhosNdkFileStorage> buffer);

protected:
    void Destroy() override
    {
        delete this;
    }

private:
    uint64_t index_ { 0 };
    std::shared_ptr<OhosNdkFileStorage> buffer_;
    BASE_NS::refcnt_ptr<OhosNdkResMgr> fileResMgr_;
};

CORE_END_NAMESPACE()
#endif // CORE_OS_OHOS_NDK_OHOS_FILE_NDK_H
