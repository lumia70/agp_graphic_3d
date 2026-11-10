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
    float camLookUp[3] = { 0, 1, 0 };
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
        const sog::Json* up = camInfo->get("lookUp");
        for (int i = 0; i < 3 && pos && (size_t)i < pos->items.size(); i++) {
            camPos[i] = (float)pos->items[i].number;
        }
        for (int i = 0; i < 3 && look && (size_t)i < look->items.size(); i++) {
            camLookAt[i] = (float)look->items[i].number;
        }
        for (int i = 0; i < 3 && up && (size_t)i < up->items.size(); i++) {
            camLookUp[i] = (float)up->items[i].number;
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
    LOGI("  budget %{public}u gaussians, cache %{public}u, camera (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) up (%.1f,%.1f,%.1f)",
        maxRenderCount, cacheCount, camPos[0], camPos[1], camPos[2], camLookAt[0], camLookAt[1],
        camLookAt[2], camLookUp[0], camLookUp[1], camLookUp[2]);
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

// Level for a distance with a global budget scale: the scene's LOD distances are
// multiplied by one scalar, so the whole scene moves between levels together
// instead of leaves being upgraded one by one by an importance sort. Flipping a
// leaf only happens when its distance crosses scale * distances[k], which is a
// thin shell in space -- much less visible than an arbitrary budget-driven
// "these 300 leaves upgrade now".
inline int PickLevelAt(const Scene& scene, float dist, float scale)
{
    for (int level = 0; level < scene.levelCount; level++) {
        if (dist < scene.distances[level] * scale) {
            return level;
        }
    }
    return scene.levelCount - 1;
}

inline float LeafRadius(const Leaf& leaf)
{
    const float ex = leaf.bmax[0] - leaf.bmin[0];
    const float ey = leaf.bmax[1] - leaf.bmin[1];
    const float ez = leaf.bmax[2] - leaf.bmin[2];
    return 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
}

// Frustum cull + distance only; levels are assigned once the global scale is
// known (see AssignLevels).
inline void CullVisible(const Scene& scene, const float view[16], const float proj[16],
    const float camPos[3], Selection& out)
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
        item.dist = dist;
        // Apparent size (bound radius / distance). Used only to order the idle
        // refinement quota, not to pick levels.
        item.score = LeafRadius(leaf) / std::max(dist, 0.001f);
        out.items.push_back(item);
    }

    // Assembly order: nearest first, so blocks are allocated (and refinement
    // consumed) from the foreground outwards.
    std::sort(out.items.begin(), out.items.end(),
        [](const Item& a, const Item& b) { return a.dist < b.dist; });
}

inline uint32_t LeafCountAt(const Scene& scene, uint32_t leafIndex, int level)
{
    const Leaf& leaf = scene.leaves[leafIndex];
    if (level >= leaf.levels) {
        level = leaf.levels - 1;
    }
    if (level < 0) {
        level = 0;
    }
    return leaf.lods[level].count;
}

// Gaussians the selection would draw at a given scale.
inline uint32_t ScaleCost(const Scene& scene, const Selection& sel, float scale)
{
    uint64_t total = 0;
    for (const auto& it : sel.items) {
        total += LeafCountAt(scene, it.leaf, PickLevelAt(scene, it.dist, scale));
    }
    return (uint32_t)std::min<uint64_t>(total, 0xFFFFFFFFull);
}

// Per-leaf levels + totals for a given global scale.
inline void AssignLevels(const Scene& scene, Selection& out, float scale)
{
    for (auto& v : out.perLevelGaussians) {
        v = 0;
    }
    for (auto& v : out.perLevelLeaves) {
        v = 0;
    }
    uint32_t total = 0;
    for (auto& it : out.items) {
        const Leaf& leaf = scene.leaves[it.leaf];
        int level = PickLevelAt(scene, it.dist, scale);
        if (level >= leaf.levels) {
            level = leaf.levels - 1;
        }
        if (level < 0) {
            level = 0;
        }
        it.level = (uint8_t)level;
        const uint32_t count = leaf.lods[level].count;
        total += count;
        out.perLevelGaussians[level] += count;
        out.perLevelLeaves[level]++;
    }
    out.total = total;
}

// Largest global scale whose selection fits the budget. The cost is monotone in
// the scale (a larger scale makes every threshold larger, so leaves land on
// finer levels), so a bisection on log2 finds it in ~20 passes over the visible
// leaves. playcanvas/gsplat-unified does the same with a ln(distance) histogram;
// with per-leaf LOD distances available a direct search is simpler and exact.
inline float ChooseScale(const Scene& scene, const Selection& sel, uint32_t budget)
{
    constexpr float kMinScale = 1.0f / 32.0f;
    constexpr float kMaxScale = 64.0f;
    if (sel.items.empty()) {
        return 1.0f;
    }
    if (budget == 0 || ScaleCost(scene, sel, kMinScale) > budget) {
        return kMinScale;  // even the coarsest levels overshoot: nothing to spend
    }
    if (ScaleCost(scene, sel, kMaxScale) <= budget) {
        return kMaxScale;  // everything can be at its finest level
    }
    float lo = std::log2(kMinScale);  // fits
    float hi = std::log2(kMaxScale);  // does not fit
    for (int i = 0; i < 20; i++) {
        const float mid = 0.5f * (lo + hi);
        if (ScaleCost(scene, sel, std::exp2(mid)) <= budget) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return std::exp2(lo);
}

} // namespace lod
