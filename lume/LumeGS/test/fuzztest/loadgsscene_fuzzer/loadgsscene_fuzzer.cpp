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

#include "loadgsscene_fuzzer.h"

#include <fuzzer/FuzzedDataProvider.h>
#include <vector>
#include <string>
#include <cstdio>

namespace GS {

// Global mock environment instances
static MockFuzz::MockFileManager* g_mockFileManager = nullptr;
static MockFuzz::MockECS* g_mockECS = nullptr;

bool DoLoadGsScene(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return false;
    }

    if (g_mockFileManager == nullptr || g_mockECS == nullptr) {
        return false;
    }

    FuzzedDataProvider fdp(data, size);

    BASE_NS::string fileName = "fuzz_test_scene";

    // ConsumeBool from the tail of seed data first, so overwriteFileData
    // reflects the last byte's lowest bit (e.g. 0xFF -> true, 0x00 -> false).
    bool overwriteFileData = fdp.ConsumeBool();

    size_t fileOffset = fdp.ConsumeIntegral<size_t>() % 1024;
    uint8_t flags = fdp.ConsumeIntegral<uint8_t>();
    uint8_t sceneIndex = fdp.ConsumeIntegral<uint8_t>();
    uint32_t timeBudget = fdp.ConsumeIntegral<uint32_t>();

    if (overwriteFileData) {
        CORE_NS::IFile::Ptr file = g_mockFileManager->CreateFile(fileName);
        if (file == nullptr) {
            return false;
        }
        std::vector<uint8_t> sceneData = fdp.ConsumeRemainingBytes<uint8_t>();
        file->Write(sceneData.data(), sceneData.size());
    }

    GsSceneLoader loader(*g_mockFileManager);

    bool result = false;
    CORE3D_NS::ISceneLoader::Result loadResult = loader.Load(fileName);
    result = (loadResult.data != nullptr);

    if (result) {
        CORE3D_NS::ISceneImporter::Ptr importer = loader.CreateSceneImporter(*g_mockECS);
        if (importer != nullptr) {
            CORE3D_NS::ISceneLoader::Result loadResultWithOffset = loader.Load(fileName, fileOffset);
            CORE3D_NS::ISceneImporter::Result importerResult = importer->GetResult();
            CORE3D_NS::ResourceImportFlags importFlags = static_cast<CORE3D_NS::ResourceImportFlags>(flags);
            importer->ImportResources(loadResultWithOffset.data, importFlags);
            CORE_NS::Entity entity = importer->ImportScene(sceneIndex);
            importer->Execute(timeBudget);

            bool isCompleted = importer->IsCompleted();
            (void)isCompleted;

            importer->Cancel();
        }
    }

    return result;
}

} // namespace GS

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc;
    (void)argv;

    GS::g_mockFileManager = new MockFuzz::MockFileManager();
    GS::g_mockECS = new MockFuzz::MockECS();

    GS::g_mockECS->RegisterManager(new MockFuzz::MockUriComponentManager(GS::g_mockECS));
    GS::g_mockECS->RegisterManager(new MockFuzz::MockGsComponentManager(GS::g_mockECS));
    GS::g_mockECS->RegisterSystem(new MockFuzz::MockNodeSystem(GS::g_mockECS));

    GS::g_mockECS->Initialize();

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    GS::DoLoadGsScene(data, size);
    return 0;
}

extern "C" int LLVMFuzzerTearDown()
{
    delete GS::g_mockFileManager;
    delete GS::g_mockECS;

    GS::g_mockFileManager = nullptr;
    GS::g_mockECS = nullptr;

    return 0;
}
