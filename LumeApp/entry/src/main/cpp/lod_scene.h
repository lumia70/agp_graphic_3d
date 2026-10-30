/*
 * lod_scene.h - LOD scene metadata (default.scene.json + lod-meta.json).
 *
 * Scene layout produced by the AGP toolchain:
 *   default.scene.json   mobileLodDistances / maxRenderCountOfMobile / cameraInfo
 *   lod-meta.json        lodLevels, filenames[], tree (binary spatial hierarchy)
 *
 * Internal tree nodes carry only `bound` + `children`; leaves carry `bound` and
 *   lods: { "0".."N-1": { file, offset, count } }
 * where `file` indexes `filenames[]` and offset/count select a gaussian range
 * from that tile. Each tile file is a concatenation of its leaves' ranges for
 * one LOD level (verified: file count == sum of leaf counts).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sog_loader.h"

namespace lod {

struct Slice {
    int32_t file = -1;
    uint32_t offset = 0;
    uint32_t count = 0;
};

struct Leaf {
    float bmin[3] = { 0, 0, 0 };
    float bmax[3] = { 0, 0, 0 };
    Slice lods[8];
    int levels = 0;
};

struct Scene {
    std::string dir;
    int levelCount = 0;
    float distances[8] = { 0 };
    uint32_t maxRenderCount = 1000000;
    uint32_t cacheCount = 5000000;
    float camPos[3] = { 0, 0, 0 };
    float camLookAt[3] = { 0, 0, -1 };
    std::vector<std::string> files;
    std::vector<Leaf> leaves;
    float boundsMin[3] = { 1e30f, 1e30f, 1e30f };
    float boundsMax[3] = { -1e30f, -1e30f, -1e30f };
    size_t totalGaussians[8] = { 0 };

    bool Load(const std::string& sceneDir);
};

struct Item {
    uint32_t leaf = 0;
    uint8_t level = 0;
    float dist = 0.0f;
    // Screen importance: leaf radius / distance. Larger = more visible area, so
    // it gets budget priority for finer levels.
    float score = 0.0f;
};

struct Selection {
    std::vector<Item> items;
    uint32_t total = 0;
    uint32_t considered = 0;
    uint32_t culled = 0;
    uint32_t perLevelGaussians[8] = { 0 };
    uint32_t perLevelLeaves[8] = { 0 };
};

inline bool ReadTextFile(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    out.resize((size_t)size);
    const size_t read = fread(&out[0], 1, out.size(), f);
    fclose(f);
    return read == out.size();
}

inline const sog::Json* Find(const sog::Json& node, const char* key)
{
    return node.get(key);
}

inline float NumberOr(const sog::Json* node, float fallback)
{
    return (node && node->type == sog::Json::Number) ? (float)node->number : fallback;
}

inline bool ParseLeaf(const sog::Json& node, Leaf& leaf, int levelCount)
{
    const sog::Json* bound = node.get("bound");
    if (!bound) {
        return false;
    }
    const sog::Json* mn = bound->get("min");
    const sog::Json* mx = bound->get("max");
    if (!mn || !mx || mn->items.size() < 3 || mx->items.size() < 3) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        leaf.bmin[i] = (float)mn->items[i].number;
        leaf.bmax[i] = (float)mx->items[i].number;
    }
    const sog::Json* lods = node.get("lods");
    if (!lods) {
        return false;
    }
    leaf.levels = 0;
    for (int level = 0; level < levelCount && level < 8; level++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", level);
        const sog::Json* entry = lods->get(key);
        if (!entry) {
            continue;
        }
        const sog::Json* file = entry->get("file");
        const sog::Json* offset = entry->get("offset");
        const sog::Json* count = entry->get("count");
        if (!file || !offset || !count) {
            continue;
        }
        leaf.lods[level].file = (int32_t)file->number;
        leaf.lods[level].offset = (uint32_t)offset->number;
        leaf.lods[level].count = (uint32_t)count->number;
        leaf.levels = level + 1;
    }
    return leaf.levels > 0;
}

inline void FlattenTree(const sog::Json& node, int levelCount, std::vector<Leaf>& out)
{
    const sog::Json* children = node.get("children");
    if (children && !children->items.empty()) {
        for (const auto& child : children->items) {
            FlattenTree(child, levelCount, out);
        }
        return;
    }
    Leaf leaf;
    if (ParseLeaf(node, leaf, levelCount)) {
        out.push_back(leaf);
    }
}

inline bool Scene::Load(const std::string& sceneDir)
{
    dir = sceneDir;
    std::string sceneText;
    std::string metaText;
    if (!ReadTextFile(sceneDir + "/default.scene.json", sceneText) ||
        !ReadTextFile(sceneDir + "/lod-meta.json", metaText)) {
        LOGE("LOD: missing default.scene.json / lod-meta.json in '%{public}s'", sceneDir.c_str());
        return false;
    }

    sog::Json sceneJson;
    sog::Json metaJson;
    {
        sog::JsonParser parser(sceneText.c_str(), sceneText.c_str() + sceneText.size());
        if (!parser.Parse(sceneJson)) {
            LOGE("LOD: default.scene.json parse failed");
            return false;
        }
        sog::JsonParser parser2(metaText.c_str(), metaText.c_str() + metaText.size());
        if (!parser2.Parse(metaJson)) {
            LOGE("LOD: lod-meta.json parse failed");
            return false;
        }
    }

    const sog::Json* distances = sceneJson.get("mobileLodDistances");
    levelCount = (int)NumberOr(metaJson.get("lodLevels"), 1.0f);
    if (levelCount < 1) {
        levelCount = 1;
    }
    if (levelCount > 8) {
        levelCount = 8;
    }
    for (int i = 0; i < levelCount; i++) {
        const float def = (i == levelCount - 1) ? 1e9f : (float)(i + 1) * 100.0f;
        Scene::distances[i] = distances && (size_t)i < distances->items.size()
                                   ? (float)distances->items[i].number
                                   : def;
    }
    maxRenderCount = (uint32_t)NumberOr(sceneJson.get("maxRenderCountOfMobile"), 1000000.0f);
    if (maxRenderCount == 0) {
        maxRenderCount = 1000000;
    }
    cacheCount = (uint32_t)NumberOr(sceneJson.get("mobileLodCacheCount"), 5000000.0f);

    const sog::Json* camInfo = sceneJson.get("cameraInfo");
    if (camInfo) {
        const sog::Json* pos = camInfo->get("position");
        const sog::Json* look = camInfo->get("lookAt");
        for (int i = 0; i < 3 && pos && (size_t)i < pos->items.size(); i++) {
            camPos[i] = (float)pos->items[i].number;
        }
        for (int i = 0; i < 3 && look && (size_t)i < look->items.size(); i++) {
            camLookAt[i] = (float)look->items[i].number;
        }
    }

    const sog::Json* filesJson = metaJson.get("filenames");
    if (!filesJson) {
        LOGE("LOD: lod-meta.json has no filenames");
        return false;
    }
    files.clear();
    files.reserve(filesJson->items.size());
    for (const auto& f : filesJson->items) {
        files.push_back(f.text);
    }

    const sog::Json* tree = metaJson.get("tree");
    if (!tree) {
        LOGE("LOD: lod-meta.json has no tree");
        return false;
    }
    leaves.clear();
    FlattenTree(*tree, levelCount, leaves);

    for (auto& l : totalGaussians) {
        l = 0;
    }
    for (const auto& leaf : leaves) {
        for (int i = 0; i < 3; i++) {
            boundsMin[i] = std::min(boundsMin[i], leaf.bmin[i]);
            boundsMax[i] = std::max(boundsMax[i], leaf.bmax[i]);
        }
        for (int level = 0; level < leaf.levels; level++) {
            totalGaussians[level] += leaf.lods[level].count;
        }
    }

    LOGI("LOD '%{public}s': %{public}zu leaves, %{public}d levels, %{public}zu files",
        sceneDir.c_str(), leaves.size(), levelCount, files.size());
    for (int level = 0; level < levelCount; level++) {
        LOGI("  LOD%{public}d: %{public}zu gaussians (dist < %.0f)", level, totalGaussians[level],
            Scene::distances[level]);
    }
    LOGI("  budget %{public}u gaussians, cache %{public}u, camera (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f)",
        maxRenderCount, cacheCount, camPos[0], camPos[1], camPos[2], camLookAt[0], camLookAt[1],
        camLookAt[2]);
    return true;
}

inline float DistanceToAabb(const float p[3], const Leaf& leaf)
{
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float v = p[i];
        const float lo = leaf.bmin[i];
        const float hi = leaf.bmax[i];
        const float delta = v < lo ? (lo - v) : (v > hi ? (v - hi) : 0.0f);
        d2 += delta * delta;
    }
    return std::sqrt(d2);
}

// Frustum planes extracted from a column-major view-projection matrix.
inline void FrustumPlanes(const float vp[16], float planes[6][4])
{
    // rows of the column-major matrix
    const float* m = vp;
    const float r0[4] = { m[0], m[4], m[8], m[12] };
    const float r1[4] = { m[1], m[5], m[9], m[13] };
    const float r2[4] = { m[2], m[6], m[10], m[14] };
    const float r3[4] = { m[3], m[7], m[11], m[15] };
    for (int i = 0; i < 4; i++) {
        planes[0][i] = r3[i] + r0[i];  // left
        planes[1][i] = r3[i] - r0[i];  // right
        planes[2][i] = r3[i] + r1[i];  // bottom
        planes[3][i] = r3[i] - r1[i];  // top
        planes[4][i] = r3[i] + r2[i];  // near
        planes[5][i] = r3[i] - r2[i];  // far
    }
}

inline bool AabbOutside(const float planes[6][4], const Leaf& leaf)
{
    for (int p = 0; p < 6; p++) {
        // Positive vertex: the AABB corner furthest along the plane normal.
        float x = planes[p][0] >= 0.0f ? leaf.bmax[0] : leaf.bmin[0];
        float y = planes[p][1] >= 0.0f ? leaf.bmax[1] : leaf.bmin[1];
        float z = planes[p][2] >= 0.0f ? leaf.bmax[2] : leaf.bmin[2];
        if (planes[p][0] * x + planes[p][1] * y + planes[p][2] * z + planes[p][3] < 0.0f) {
            return true;
        }
    }
    return false;
}

// Level for a fresh leaf (no previous selection).
inline int PickLevel(const Scene& scene, float dist)
{
    for (int level = 0; level < scene.levelCount; level++) {
        if (dist < scene.distances[level]) {
            return level;
        }
    }
    return scene.levelCount - 1;
}

// Hysteresis: only switch away from the previous level once the distance
// crosses the threshold by 15%, which stops leaves oscillating between two
// levels (visible as geometry popping while the camera drifts).
inline int PickLevelHysteresis(const Scene& scene, float dist, int previous)
{
    if (previous < 0 || previous >= scene.levelCount) {
        return PickLevel(scene, dist);
    }
    const float kHysteresis = 1.15f;
    // Coarser (previous+1) only when clearly beyond the current threshold.
    const float currentMax = scene.distances[previous];
    if (dist < currentMax * kHysteresis) {
        // Still inside the current level (with margin): go finer only when
        // clearly below the finer threshold.
        if (previous > 0) {
            const float finerMax = scene.distances[previous - 1];
            if (dist < finerMax / kHysteresis) {
                return previous - 1;
            }
        }
        return previous;
    }
    // Coarse enough for the next level?
    if (previous + 1 < scene.levelCount) {
        return PickLevel(scene, dist);
    }
    return previous;
}

// Frustum cull + per-leaf distance LOD, then coarsen the farthest leaves until
// the gaussian budget is met.
inline void SelectVisible(const Scene& scene, const float view[16], const float proj[16],
    const float camPos[3], uint32_t budget, const int8_t* previousLevels, Selection& out)
{
    out.items.clear();
    out.total = 0;
    out.considered = 0;
    out.culled = 0;
    if (scene.leaves.empty()) {
        return;
    }

    float vp[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += proj[k * 4 + r] * view[c * 4 + k];
            }
            vp[c * 4 + r] = sum;
        }
    }
    float planes[6][4];
    FrustumPlanes(vp, planes);

    out.items.reserve(scene.leaves.size());
    for (uint32_t i = 0; i < scene.leaves.size(); i++) {
        const Leaf& leaf = scene.leaves[i];
        out.considered++;
        if (AabbOutside(planes, leaf)) {
            out.culled++;
            continue;
        }
        const float dist = DistanceToAabb(camPos, leaf);
        Item item;
        item.leaf = i;
        item.level = (uint8_t)PickLevelHysteresis(scene, dist,
            previousLevels ? previousLevels[i] : -1);
        item.dist = dist;
        out.items.push_back(item);
    }

    // Importance = apparent size (bound radius / distance). Budget allocation
    // works on this order: the most "expensive on screen" leaves get the finest
    // levels the budget allows, the least important ones are coarsened first.
    for (auto& it : out.items) {
        const Leaf& leaf = scene.leaves[it.leaf];
        const float ex = leaf.bmax[0] - leaf.bmin[0];
        const float ey = leaf.bmax[1] - leaf.bmin[1];
        const float ez = leaf.bmax[2] - leaf.bmin[2];
        const float radius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
        it.score = radius / std::max(it.dist, 0.001f);
    }
    std::sort(out.items.begin(), out.items.end(),
        [](const Item& a, const Item& b) { return a.score > b.score; });

    uint32_t total = 0;
    for (const auto& it : out.items) {
        total += scene.leaves[it.leaf].lods[it.level].count;
    }

    if (budget > 0) {
        // 1) Over budget: coarsen the least important leaves first; a leaf that
        //    cannot fit at any level is dropped for this frame.
        if (total > budget) {
            for (size_t i = out.items.size(); i-- > 0 && total > budget;) {
                Item& it = out.items[i];
                const Leaf& leaf = scene.leaves[it.leaf];
                total -= leaf.lods[it.level].count;
                bool placed = false;
                for (int level = it.level + 1; level < leaf.levels; level++) {
                    const uint32_t count = leaf.lods[level].count;
                    if (total + count <= budget) {
                        it.level = (uint8_t)level;
                        total += count;
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    it.level = 0xFF;
                }
            }
            out.items.erase(std::remove_if(out.items.begin(), out.items.end(),
                                [](const Item& it) { return it.level == 0xFF; }),
                out.items.end());
        } else {
            // 2) Under budget: spend the leftover on the most important leaves,
            //    upgrading them one level at a time while they fit.
            bool upgraded = true;
            while (upgraded) {
                upgraded = false;
                for (auto& it : out.items) {
                    if (it.level == 0) {
                        continue;  // already the finest
                    }
                    const Leaf& leaf = scene.leaves[it.leaf];
                    const uint32_t current = leaf.lods[it.level].count;
                    const uint32_t finer = leaf.lods[it.level - 1].count;
                    if (finer <= current) {
                        continue;
                    }
                    const uint32_t delta = finer - current;
                    if (total + delta <= budget) {
                        it.level = (uint8_t)(it.level - 1);
                        total += delta;
                        upgraded = true;
                    }
                }
            }
        }
        total = 0;
        for (const auto& it : out.items) {
            total += scene.leaves[it.leaf].lods[it.level].count;
        }
    }

    // Assembly order: nearest first, so refinement appears from the foreground
    // outwards instead of in arbitrary importance blocks.
    std::sort(out.items.begin(), out.items.end(),
        [](const Item& a, const Item& b) { return a.dist < b.dist; });

    out.total = total;
    for (auto& v : out.perLevelGaussians) {
        v = 0;
    }
    for (auto& v : out.perLevelLeaves) {
        v = 0;
    }
    for (const auto& it : out.items) {
        const uint32_t count = scene.leaves[it.leaf].lods[it.level].count;
        if (it.level < 8) {
            out.perLevelGaussians[it.level] += count;
            out.perLevelLeaves[it.level]++;
        }
    }
}

} // namespace lod
