/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_GPU_VISION_DEVICE_H
#define ANIMUS_GPU_VISION_DEVICE_H

#include "Camera.h"
#include <cstdint>
#include <cstring>
#include <math.h>

/// **The camera's caster on the GPU** (camera-vision.GPU.md, G1 and G2): the scene's memory layout and every step of a
/// pixel's ray, written once and compiled twice -- by hipcc into libforge-gpu.so (Device/Vision.hip, one thread a
/// ray), and by the host compiler into the worldserver, where the same functions run over the host copy of the scene
/// (the tests, and `forge camera diff`'s emulated column). Plain C++ over 32-bit words and raw pointers: no HIP type,
/// no std call a device cannot make, nothing but `__host__ __device__`, so nvcc builds it behind a thin HIP-to-CUDA
/// header too.
///
/// It is a port, line for line, of what the CPU caster calls, so a GPU frame is the CPU's frame:
/// - the order and arithmetic of Vision/VisionCaster.cpp's Nearest, Reach, CastTerrain, RayCylinder, ObjectiveFlag
///   and EncodePixel, and Camera.h's PixelAngles, Direction and Upscale;
/// - the collision trees exactly as the CPU walks them, from the CPU's own data: the static tree's BIH over the map's
///   spawns (StaticMapTree), each spawn through its transform into its model's space (ModelInstance), each model's
///   BIH over its groups (WorldModel), each group's BIH over its triangles (GroupModel), the triangle test with its
///   1e-5 determinant (IntersectTriangle), the WMO liquids (WmoLiquid::IntersectRay), G3D's ray-box test and the
///   dynamic tree's 64 x 64 cell walk (RegularGrid2D, quirks and all) over the doors filed in each cell;
/// - the constants are Camera.h's own.
///
/// Where the two can still differ is the last bit of a float: the device rounds its own cos/sin/log, and the host
/// build contracts multiply-adds (-march=native has FMA, clang's -ffp-contract=on) where the kernels, built with
/// -ffp-contract=off, do not. `forge camera diff` measures what that leaves.
#if defined(__HIPCC__) || defined(__CUDACC__)
#define FORGE_HD __host__ __device__
#else
#define FORGE_HD
#endif

#if defined(__HIP_DEVICE_COMPILE__) || defined(__CUDA_ARCH__)
#define FORGE_DEVICE_PASS 1
#else
#define FORGE_DEVICE_PASS 0
#endif

namespace Animus::GpuVision
{
    // ---- The scene's layout --------------------------------------------------------------------------------------
    //
    // Everything lives in arrays of 32-bit words (floats stored by their bits), addressed by word offsets from the
    // array's start; offset 0 is never a record, so 0 reads "none".
    //
    // A BIH record (BIH_WORDS): its bounds' low x, y, z and high x, y, z, then the word offsets of its node array and
    // of its object index array (BIH::GetNodes, BIH::GetObjects), copied as they are.
    //
    // A model (a WorldModel; MODEL_HEADER then the groups): its group count, its group BIH, then GROUP_WORDS a group:
    //   0-5 the group's bound (GroupModel::GetBound), 6 its triangle count, 7 its vertex offset (x, y, z a vertex),
    //   8 its triangle offset (three vertex indices a triangle), 9-16 its mesh BIH, 17 its liquid's offset or 0.
    // A liquid (LIQUID_HEADER then its data): tiles x, tiles y, the corner's x, y, z, its LiquidType, 1 when that type
    //   is magma or slime, 1 when it has tile flags; then (tiles x + 1) * (tiles y + 1) heights (one, with no flags),
    //   then the tile flags, four bytes a word.
    //
    // An instance (INSTANCE_WORDS; a static ModelInstance or a door's GameObjectModel): its model's offset in the
    // model pool, its spawn flags, its position, its inverse rotation (row-major, Matrix3::elt[r][c]), its inverse
    // scale and scale, its bound, and for a door its phase mask and whether its owner is spawned.
    //
    // A terrain grid (TERRAIN_WORDS): whether it has heights, its highest point, whether it has liquid; then the
    // 129 x 129 corner heights, the 128 x 128 centre heights and liquid levels, and the cells' flags, a byte a cell
    // (CELL_SOLID, CELL_LIQUID, CELL_DEADLY), each indexed as GridTerrainData::GetCellHeights indexes them.
    constexpr uint32_t BIH_WORDS = 8;
    constexpr uint32_t BIH_NODES = 6;
    constexpr uint32_t BIH_OBJECTS = 7;

    constexpr uint32_t MODEL_GROUPS = 0;
    constexpr uint32_t MODEL_TREE = 1;
    constexpr uint32_t MODEL_HEADER = 1 + BIH_WORDS;

    constexpr uint32_t GROUP_BOUND = 0;
    constexpr uint32_t GROUP_TRIANGLES = 6;
    constexpr uint32_t GROUP_VERTEX_OFFSET = 7;
    constexpr uint32_t GROUP_TRIANGLE_OFFSET = 8;
    constexpr uint32_t GROUP_TREE = 9;
    constexpr uint32_t GROUP_LIQUID = GROUP_TREE + BIH_WORDS;
    constexpr uint32_t GROUP_WORDS = GROUP_LIQUID + 1;

    constexpr uint32_t LIQUID_TILES_X = 0;
    constexpr uint32_t LIQUID_TILES_Y = 1;
    constexpr uint32_t LIQUID_CORNER = 2;
    constexpr uint32_t LIQUID_TYPE = 5;
    constexpr uint32_t LIQUID_DEADLY = 6;
    constexpr uint32_t LIQUID_HAS_FLAGS = 7;
    constexpr uint32_t LIQUID_HEADER = 8;

    constexpr uint32_t INSTANCE_MODEL = 0;
    constexpr uint32_t INSTANCE_FLAGS = 1;
    constexpr uint32_t INSTANCE_POS = 2;
    constexpr uint32_t INSTANCE_INV_ROT = 5;
    constexpr uint32_t INSTANCE_INV_SCALE = 14;
    constexpr uint32_t INSTANCE_SCALE = 15;
    constexpr uint32_t INSTANCE_BOUND = 16;
    constexpr uint32_t INSTANCE_PHASE = 22;
    constexpr uint32_t INSTANCE_SPAWNED = 23;
    constexpr uint32_t INSTANCE_WORDS = 24;
    /// VMAP::MOD_M2: an M2 has no liquid.
    constexpr uint32_t SPAWN_M2 = 1;
    /// A static tree slot whose spawn has no model (its tile never loaded) in the slot table.
    constexpr uint32_t NO_INSTANCE = 0xFFFFFFFFu;

    constexpr uint32_t TERRAIN_CELLS = 128;
    constexpr uint32_t TERRAIN_V9 = 129;
    constexpr uint32_t TERRAIN_HEIGHTS = 0;
    constexpr uint32_t TERRAIN_MAX = 1;
    constexpr uint32_t TERRAIN_LIQUID = 2;
    constexpr uint32_t TERRAIN_HEADER = 4;
    constexpr uint32_t TERRAIN_CORNERS = TERRAIN_HEADER;
    constexpr uint32_t TERRAIN_CENTRES = TERRAIN_CORNERS + TERRAIN_V9 * TERRAIN_V9;
    constexpr uint32_t TERRAIN_LEVELS = TERRAIN_CENTRES + TERRAIN_CELLS * TERRAIN_CELLS;
    constexpr uint32_t TERRAIN_FLAGS = TERRAIN_LEVELS + TERRAIN_CELLS * TERRAIN_CELLS;
    constexpr uint32_t TERRAIN_WORDS = TERRAIN_FLAGS + TERRAIN_CELLS * TERRAIN_CELLS / 4;
    constexpr uint32_t CELL_SOLID = 1;
    constexpr uint32_t CELL_LIQUID = 2;
    constexpr uint32_t CELL_DEADLY = 4;

    /// The dynamic tree's grid (RegularGrid2D): 64 x 64 cells over the map, a door filed in up to 9 of them.
    constexpr int32_t DOOR_GRID = 64;
    constexpr uint32_t DOOR_GRID_CELLS = uint32_t(DOOR_GRID * DOOR_GRID);

    /// A grid created with no terrain data (a WMO-only map): loaded, with no heights and no liquid.
    FORGE_HD inline uint32_t const* NoTerrain() { return reinterpret_cast<uint32_t const*>(uintptr_t(1)); }

    /// One map instance as the kernel reads it (pointers into the device's copies, or the host's for the emulation).
    struct SceneView
    {
        /// GRIDS x GRIDS grids, [tileX * GRIDS + tileY]: null not created, NoTerrain(), or the terrain grid.
        uint32_t const* const* Grids = nullptr;
        /// The static tree: a BIH over its loaded spawns (a record at word 0 of Top), the instance index of each
        /// of its objects (Slots, or NO_INSTANCE), the same for those with a WMO liquid (LiquidTop, LiquidSlots),
        /// the instances and the model pool. Null Top: the map has no static tree.
        uint32_t const* Top = nullptr;
        uint32_t const* Slots = nullptr;
        uint32_t const* LiquidTop = nullptr;
        uint32_t const* LiquidSlots = nullptr;
        uint32_t const* Instances = nullptr;
        uint32_t const* Models = nullptr;
        /// The doors: DoorCells holds DOOR_GRID_CELLS (offset, count) pairs into the door index list after them;
        /// DoorModels is their model pool.
        uint32_t const* Doors = nullptr;
        uint32_t const* DoorCells = nullptr;
        uint32_t const* DoorModels = nullptr;
        uint32_t DoorCount = 0;
        uint32_t Pad = 0;
    };

    /// A unit's cylinder (Vision::UnitShape without the seat's own: the host drops it).
    struct DeviceUnit
    {
        float X;
        float Y;
        float Z;
        float Radius;
        float Height;
        uint32_t Hostile;
    };

    /// One frame to cast: the rig the CPU placed (PlaceCamera: the boom is one CPU ray), the size it is cast at and
    /// the canonical size it is scaled up into, the objective, its units and its scene, and where its bytes go.
    struct FrameRequest
    {
        float CameraX;
        float CameraY;
        float CameraZ;
        float Azimuth;
        float Elevation;
        float FeetZ;
        float ObjectiveX;
        float ObjectiveY;
        float ObjectiveZ;
        uint32_t HasObjective;
        float FovH;
        float FovV;
        uint32_t CastWidth;
        uint32_t CastHeight;
        uint32_t Width;
        uint32_t Height;
        uint32_t Scene;
        uint32_t PhaseMask;
        uint32_t UnitOffset;
        uint32_t UnitCount;
        /// Byte offsets: the cast frame (in the scratch buffer when it is scaled up, else straight into the image)
        /// and the canonical one in the image buffer.
        uint64_t ScratchOffset;
        uint64_t ImageOffset;
    };

    // ---- Words, floats and the few math calls a device has too -------------------------------------------------

    FORGE_HD inline float AsFloat(uint32_t bits)
    {
#if FORGE_DEVICE_PASS
        return __uint_as_float(bits);
#else
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
#endif
    }

    FORGE_HD inline uint32_t AsBits(float value)
    {
#if FORGE_DEVICE_PASS
        return __float_as_uint(value);
#else
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
#endif
    }

    FORGE_HD inline float Inf() { return AsFloat(0x7F800000u); }
    FORGE_HD inline bool Finite(float value) { return (AsBits(value) & 0x7F800000u) != 0x7F800000u; }
    /// std::min / std::max / std::clamp as the standard writes them, for the same answer on a tie or a NaN.
    FORGE_HD inline float Min(float a, float b) { return b < a ? b : a; }
    FORGE_HD inline float Max(float a, float b) { return a < b ? b : a; }
    FORGE_HD inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
    FORGE_HD inline int32_t ClampI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (hi < v ? hi : v); }

    struct V3
    {
        float X;
        float Y;
        float Z;
    };

    FORGE_HD inline V3 Make(float x, float y, float z) { V3 v; v.X = x; v.Y = y; v.Z = z; return v; }
    FORGE_HD inline V3 operator+(V3 a, V3 b) { return Make(a.X + b.X, a.Y + b.Y, a.Z + b.Z); }
    FORGE_HD inline V3 operator-(V3 a, V3 b) { return Make(a.X - b.X, a.Y - b.Y, a.Z - b.Z); }
    FORGE_HD inline V3 operator*(V3 a, float s) { return Make(a.X * s, a.Y * s, a.Z * s); }
    /// G3D's and Vision's dot, cross and length, term for term.
    FORGE_HD inline float Dot(V3 a, V3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    FORGE_HD inline V3 Cross(V3 a, V3 b)
    {
        return Make(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    }
    FORGE_HD inline float Length(V3 a) { return sqrtf(a.X * a.X + a.Y * a.Y + a.Z * a.Z); }
    FORGE_HD inline float Component(V3 v, int i) { return i == 0 ? v.X : (i == 1 ? v.Y : v.Z); }
    FORGE_HD inline V3 ReadV3(uint32_t const* w) { return Make(AsFloat(w[0]), AsFloat(w[1]), AsFloat(w[2])); }

    /// G3D::Matrix3 * Vector3, and its transpose's (the inverse rotation's inverse), as Matrix3 sums them.
    FORGE_HD inline V3 Rotate(uint32_t const* m, V3 v)
    {
        return Make(AsFloat(m[0]) * v.X + AsFloat(m[1]) * v.Y + AsFloat(m[2]) * v.Z,
            AsFloat(m[3]) * v.X + AsFloat(m[4]) * v.Y + AsFloat(m[5]) * v.Z,
            AsFloat(m[6]) * v.X + AsFloat(m[7]) * v.Y + AsFloat(m[8]) * v.Z);
    }
    FORGE_HD inline V3 RotateBack(uint32_t const* m, V3 v)
    {
        return Make(AsFloat(m[0]) * v.X + AsFloat(m[3]) * v.Y + AsFloat(m[6]) * v.Z,
            AsFloat(m[1]) * v.X + AsFloat(m[4]) * v.Y + AsFloat(m[7]) * v.Z,
            AsFloat(m[2]) * v.X + AsFloat(m[5]) * v.Y + AsFloat(m[8]) * v.Z);
    }

    /// G3D::fuzzyNe(dir, 0.0f), which takes the double overload: not within 5e-7 x (|a| + 1) of 0.
    FORGE_HD inline bool FuzzyNonZero(float value)
    {
        double const a = double(value);
        if (a == 0.0)
            return false;
        double const magnitude = (a < 0.0 ? -a : a);
        return !(magnitude <= 0.0000005 * (magnitude + 1.0));
    }

    // ---- The CPU's collision code, ported --------------------------------------------------------------------

    /// G3D::Ray::intersectionTime(AABox) != inf (CollisionDetection::collisionLocationForMovingPointFixedAABox):
    /// whether the ray, from its origin on, meets the box at all -- at any distance, as ModelInstance asks it.
    FORGE_HD inline bool RayMeetsBox(V3 origin, V3 dir, uint32_t const* bound)
    {
        float const lo[3] = { AsFloat(bound[0]), AsFloat(bound[1]), AsFloat(bound[2]) };
        float const hi[3] = { AsFloat(bound[3]), AsFloat(bound[4]), AsFloat(bound[5]) };
        float const o[3] = { origin.X, origin.Y, origin.Z };
        float const d[3] = { dir.X, dir.Y, dir.Z };
        bool inside = true;
        float maxT[3] = { -1.0f, -1.0f, -1.0f };
        for (int i = 0; i < 3; ++i)
        {
            if (o[i] < lo[i])
            {
                inside = false;
                if (AsBits(d[i]))
                    maxT[i] = (lo[i] - o[i]) / d[i];
            }
            else if (o[i] > hi[i])
            {
                inside = false;
                if (AsBits(d[i]))
                    maxT[i] = (hi[i] - o[i]) / d[i];
            }
        }
        if (inside)
            return true;
        int plane = 0;
        if (maxT[1] > maxT[plane])
            plane = 1;
        if (maxT[2] > maxT[plane])
            plane = 2;
        if (AsBits(maxT[plane]) & 0x80000000u)
            return false;
        for (int i = 0; i < 3; ++i)
        {
            if (i == plane)
                continue;
            float const at = o[i] + maxT[plane] * d[i];
            if (at < lo[i] || at > hi[i])
                return false;
        }
        return true;
    }

    struct BihStackNode
    {
        uint32_t Node;
        float Near;
        float Far;
    };
    /// BIH's MAX_STACK_SIZE: each tree's own stack on the CPU.
    constexpr int BIH_STACK = 64;
    /// A pixel's one stack, shared by the nested walks (the static tree's, a model's group tree's, a group's mesh
    /// tree's: each starts where its caller's ends). It is a pixel's whole scratch, so it is sized per launch: the
    /// kernel is compiled at each of STACK_SIZES and a launch takes the smallest that holds the worst case its
    /// scenes can need (VisionScene's depths: the spawn tree's push depth plus the deepest model's). A walk that
    /// would still push past it drops the node, as no CPU walk does, and says so: the launch's overflow count,
    /// which fails `forge camera diff`'s gate.
    constexpr int STACK_SIZES[] = { 64, 96, 128, 256 };
    constexpr int STACK_COUNT = int(sizeof(STACK_SIZES) / sizeof(STACK_SIZES[0]));
    constexpr int MAX_STACK = STACK_SIZES[STACK_COUNT - 1];

    /// The index in STACK_SIZES of the smallest stack holding `depth` entries (the largest when none does).
    FORGE_HD inline int StackIndexFor(uint32_t depth)
    {
        for (int i = 0; i < STACK_COUNT; ++i)
            if (depth <= uint32_t(STACK_SIZES[i]))
                return i;
        return STACK_COUNT - 1;
    }

    /// The free part of the shared stack, handed down to a nested walk, and where a dropped node is told.
    struct BihStack
    {
        BihStackNode* Nodes;
        int Capacity;
        bool* Overflow;
    };

    /// BIH::intersectRay (never stopping at a first hit), over the record at `bih` in `words`, on `stack`:
    /// `visit(object, maxDist, free)` for each object of each leaf the ray reaches within maxDist, which the visit
    /// may shorten; `free` is the stack above this walk's, for the visit's own walk.
    template <typename Visit>
    FORGE_HD inline void BihRayOn(BihStack stack, uint32_t const* words, uint32_t bih, V3 origin, V3 direction,
        float& maxDist, Visit&& visit)
    {
        uint32_t const* record = words + bih;
        uint32_t const* tree = words + record[BIH_NODES];
        uint32_t const* objects = words + record[BIH_OBJECTS];
        float const org[3] = { origin.X, origin.Y, origin.Z };
        float const dir[3] = { direction.X, direction.Y, direction.Z };
        float intervalMin = -1.0f;
        float intervalMax = -1.0f;
        float invDir[3];
        for (int i = 0; i < 3; ++i)
        {
            invDir[i] = 1.0f / dir[i];
            if (FuzzyNonZero(dir[i]))
            {
                float t1 = (AsFloat(record[i]) - org[i]) * invDir[i];
                float t2 = (AsFloat(record[3 + i]) - org[i]) * invDir[i];
                if (t1 > t2)
                {
                    float const swap = t1;
                    t1 = t2;
                    t2 = swap;
                }
                if (t1 > intervalMin)
                    intervalMin = t1;
                if (t2 < intervalMax || intervalMax < 0.0f)
                    intervalMax = t2;
                if (intervalMax <= 0 || intervalMin >= maxDist)
                    return;
            }
        }
        if (intervalMin > intervalMax)
            return;
        intervalMin = Max(intervalMin, 0.0f);
        intervalMax = Min(intervalMax, maxDist);

        uint32_t offsetFront[3];
        uint32_t offsetBack[3];
        uint32_t offsetFront3[3];
        uint32_t offsetBack3[3];
        for (int i = 0; i < 3; ++i)
        {
            offsetFront[i] = AsBits(dir[i]) >> 31;
            offsetBack[i] = offsetFront[i] ^ 1;
            offsetFront3[i] = offsetFront[i] * 3;
            offsetBack3[i] = offsetBack[i] * 3;
            ++offsetFront[i];
            ++offsetBack[i];
        }

        int stackPos = 0;
        int node = 0;
        while (true)
        {
            while (true)
            {
                uint32_t const tn = tree[node];
                uint32_t const axis = (tn & (3u << 30)) >> 30;
                bool const bvh2 = (tn & (1u << 29)) != 0;
                int offset = int(tn & ~(7u << 29));
                if (!bvh2)
                {
                    if (axis < 3)
                    {
                        float const tf = (AsFloat(tree[node + offsetFront[axis]]) - org[axis]) * invDir[axis];
                        float const tb = (AsFloat(tree[node + offsetBack[axis]]) - org[axis]) * invDir[axis];
                        if (tf < intervalMin && tb > intervalMax)
                            break;
                        int const back = offset + int(offsetBack3[axis]);
                        node = back;
                        if (tf < intervalMin)
                        {
                            intervalMin = (tb >= intervalMin) ? tb : intervalMin;
                            continue;
                        }
                        node = offset + int(offsetFront3[axis]);
                        if (tb > intervalMax)
                        {
                            intervalMax = (tf <= intervalMax) ? tf : intervalMax;
                            continue;
                        }
                        // The CPU's stack has no guard (an overflow there is undefined); here one is dropped, and
                        // counted.
                        if (stackPos < stack.Capacity)
                        {
                            stack.Nodes[stackPos].Node = uint32_t(back);
                            stack.Nodes[stackPos].Near = (tb >= intervalMin) ? tb : intervalMin;
                            stack.Nodes[stackPos].Far = intervalMax;
                            ++stackPos;
                        }
                        else
                            *stack.Overflow = true;
                        intervalMax = (tf <= intervalMax) ? tf : intervalMax;
                        continue;
                    }
                    int n = int(tree[node + 1]);
                    while (n > 0)
                    {
                        visit(objects[offset], maxDist, BihStack{ stack.Nodes + stackPos,
                            stack.Capacity - stackPos, stack.Overflow });
                        --n;
                        ++offset;
                    }
                    break;
                }
                if (axis > 2)
                    return;
                float const tf = (AsFloat(tree[node + offsetFront[axis]]) - org[axis]) * invDir[axis];
                float const tb = (AsFloat(tree[node + offsetBack[axis]]) - org[axis]) * invDir[axis];
                node = offset;
                intervalMin = (tf >= intervalMin) ? tf : intervalMin;
                intervalMax = (tb <= intervalMax) ? tb : intervalMax;
                if (intervalMin > intervalMax)
                    break;
            }
            while (true)
            {
                if (stackPos == 0)
                    return;
                --stackPos;
                intervalMin = stack.Nodes[stackPos].Near;
                if (maxDist < intervalMin)
                    continue;
                node = int(stack.Nodes[stackPos].Node);
                intervalMax = stack.Nodes[stackPos].Far;
                break;
            }
        }
    }

    /// BihRayOn on a stack of its own, BIH_STACK deep: `visit(object, maxDist)`.
    template <typename Visit>
    FORGE_HD inline void BihRay(uint32_t const* words, uint32_t bih, V3 origin, V3 direction, float& maxDist,
        Visit&& visit)
    {
        BihStackNode nodes[BIH_STACK];
        bool overflow = false;
        BihRayOn(BihStack{ nodes, BIH_STACK, &overflow }, words, bih, origin, direction, maxDist,
            [&](uint32_t entry, float& distance, BihStack) { visit(entry, distance); });
    }

    /// VMAP::IntersectTriangle: two-sided, a determinant under 1e-5 a miss, a hit only nearer than `distance`.
    FORGE_HD inline bool TriangleRay(V3 v0, V3 v1, V3 v2, V3 origin, V3 dir, float& distance)
    {
        V3 const e1 = v1 - v0;
        V3 const e2 = v2 - v0;
        V3 const p = Cross(dir, e2);
        float const a = Dot(e1, p);
        if (fabsf(a) < 1e-5f)
            return false;
        float const f = 1.0f / a;
        V3 const s = origin - v0;
        float const u = f * Dot(s, p);
        if (u < 0.0f || u > 1.0f)
            return false;
        V3 const q = Cross(s, e1);
        float const v = f * Dot(dir, q);
        if (v < 0.0f || u + v > 1.0f)
            return false;
        float const t = f * Dot(e2, q);
        if (t > 0.0f && t < distance)
        {
            distance = t;
            return true;
        }
        return false;
    }

    /// GroupModel::IntersectRay: the group's triangles through its mesh BIH; `normal` takes each nearer hit's
    /// (unnormalised, either side) in the model's space.
    FORGE_HD inline bool GroupRay(uint32_t const* pool, uint32_t group, V3 origin, V3 dir, float& distance,
        V3& normal, BihStack stack)
    {
        uint32_t const* g = pool + group;
        if (!g[GROUP_TRIANGLES])
            return false;
        uint32_t const* vertices = pool + g[GROUP_VERTEX_OFFSET];
        uint32_t const* triangles = pool + g[GROUP_TRIANGLE_OFFSET];
        bool hit = false;
        BihRayOn(stack, pool, group + GROUP_TREE, origin, dir, distance, [&](uint32_t entry, float& maxDist, BihStack)
        {
            uint32_t const* tri = triangles + entry * 3;
            V3 const v0 = ReadV3(vertices + tri[0] * 3);
            V3 const v1 = ReadV3(vertices + tri[1] * 3);
            V3 const v2 = ReadV3(vertices + tri[2] * 3);
            if (TriangleRay(v0, v1, v2, origin, dir, maxDist))
            {
                hit = true;
                normal = Cross(v1 - v0, v2 - v0);
            }
        });
        return hit;
    }

    FORGE_HD inline uint32_t GroupOffset(uint32_t model, uint32_t index)
    {
        return model + MODEL_HEADER + index * GROUP_WORDS;
    }

    /// WorldModel::IntersectRay: one group straight, more through the group BIH.
    FORGE_HD inline bool ModelRay(uint32_t const* pool, uint32_t model, V3 origin, V3 dir, float& distance,
        V3& normal, BihStack stack)
    {
        uint32_t const groups = pool[model + MODEL_GROUPS];
        if (groups == 1)
            return GroupRay(pool, GroupOffset(model, 0), origin, dir, distance, normal, stack);
        bool hit = false;
        BihRayOn(stack, pool, model + MODEL_TREE, origin, dir, distance, [&](uint32_t entry, float& maxDist,
            BihStack free)
        {
            if (GroupRay(pool, GroupOffset(model, entry), origin, dir, maxDist, normal, free))
                hit = true;
        });
        return hit;
    }

    /// ModelInstance::intersectRay (and GameObjectModel's, past its phase and spawn test): the ray through the
    /// instance's bound, into its model's space, and the hit's normal back out of it.
    FORGE_HD inline bool InstanceRay(uint32_t const* instance, uint32_t const* pool, V3 origin, V3 dir,
        float& maxDist, V3& normal, BihStack stack)
    {
        uint32_t const model = instance[INSTANCE_MODEL];
        if (!model)
            return false;
        if (!RayMeetsBox(origin, dir, instance + INSTANCE_BOUND))
            return false;
        uint32_t const* invRot = instance + INSTANCE_INV_ROT;
        float const invScale = AsFloat(instance[INSTANCE_INV_SCALE]);
        V3 const p = Rotate(invRot, origin - ReadV3(instance + INSTANCE_POS)) * invScale;
        V3 const modelDir = Rotate(invRot, dir);
        float distance = maxDist * invScale;
        V3 modelNormal = normal;
        bool const hit = ModelRay(pool, model, p, modelDir, distance, modelNormal, stack);
        if (hit)
        {
            distance *= AsFloat(instance[INSTANCE_SCALE]);
            maxDist = distance;
            normal = RotateBack(invRot, modelNormal);
        }
        return hit;
    }

    /// VMAP's RayTriangle in WmoLiquid::IntersectRay: two-sided, the ray parameter or -1.
    FORGE_HD inline float LiquidTriangle(V3 origin, V3 dir, V3 a, V3 b, V3 c)
    {
        V3 const e1 = b - a;
        V3 const e2 = c - a;
        V3 const p = Cross(dir, e2);
        float const det = Dot(e1, p);
        if (fabsf(det) < 1e-12f)
            return -1.0f;
        float const inv = 1.0f / det;
        V3 const s = origin - a;
        float const u = Dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
            return -1.0f;
        V3 const q = Cross(s, e1);
        float const v = Dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
            return -1.0f;
        float const t = Dot(e2, q) * inv;
        return t >= 0.0f ? t : -1.0f;
    }

    /// WmoLiquid::IntersectRay, in the model's space.
    FORGE_HD inline bool LiquidRay(uint32_t const* liquid, V3 origin, V3 dir, float& distance)
    {
        uint32_t const tilesX = liquid[LIQUID_TILES_X];
        uint32_t const tilesY = liquid[LIQUID_TILES_Y];
        uint32_t const* heights = liquid + LIQUID_HEADER;
        if (!liquid[LIQUID_HAS_FLAGS])
        {
            if (fabsf(dir.Z) < 1e-9f)
                return false;
            float const t = (AsFloat(heights[0]) - origin.Z) / dir.Z;
            if (t < 0.0f || t >= distance)
                return false;
            distance = t;
            return true;
        }
        uint32_t const* flagWords = heights + (tilesX + 1) * (tilesY + 1);
        auto const flag = [&](uint32_t index) { return (flagWords[index / 4] >> ((index % 4) * 8)) & 0xFFu; };

        float const tileSize = 533.333f / 128.f; // VMapDefinitions.h's LIQUID_TILE_SIZE
        V3 const corner = ReadV3(liquid + LIQUID_CORNER);
        float const ox = (origin.X - corner.X) / tileSize;
        float const oy = (origin.Y - corner.Y) / tileSize;
        float const dx = dir.X / tileSize;
        float const dy = dir.Y / tileSize;
        float tMin = 0.0f;
        float tMax = distance;
        auto const clip = [&](float o, float d, float high)
        {
            if (fabsf(d) < 1e-12f)
                return o >= 0.0f && o <= high;
            float t0 = -o / d;
            float t1 = (high - o) / d;
            if (t0 > t1)
            {
                float const swap = t0;
                t0 = t1;
                t1 = swap;
            }
            tMin = Max(tMin, t0);
            tMax = Min(tMax, t1);
            return tMin <= tMax;
        };
        if (!clip(ox, dx, float(tilesX)) || !clip(oy, dy, float(tilesY)))
            return false;

        int32_t ix = ClampI(int32_t(floorf(ox + dx * tMin)), 0, int32_t(tilesX) - 1);
        int32_t iy = ClampI(int32_t(floorf(oy + dy * tMin)), 0, int32_t(tilesY) - 1);
        int32_t const stepX = dx > 0.0f ? 1 : -1;
        int32_t const stepY = dy > 0.0f ? 1 : -1;
        float const inf = Inf();
        float nextX = fabsf(dx) < 1e-12f ? inf : (float(ix + (dx > 0.0f ? 1 : 0)) - ox) / dx;
        float nextY = fabsf(dy) < 1e-12f ? inf : (float(iy + (dy > 0.0f ? 1 : 0)) - oy) / dy;
        float const deltaX = fabsf(dx) < 1e-12f ? inf : 1.0f / fabsf(dx);
        float const deltaY = fabsf(dy) < 1e-12f ? inf : 1.0f / fabsf(dy);
        uint32_t const rowOffset = tilesX + 1;
        V3 const local = Make(ox, oy, origin.Z);
        V3 const localDir = Make(dx, dy, dir.Z);
        while (ix >= 0 && iy >= 0 && ix < int32_t(tilesX) && iy < int32_t(tilesY))
        {
            if ((flag(uint32_t(ix) + uint32_t(iy) * tilesX) & 0x0Fu) != 0x0Fu)
            {
                auto const at = [&](int32_t cx, int32_t cy)
                {
                    return Make(float(ix + cx), float(iy + cy),
                        AsFloat(heights[uint32_t(ix + cx) + uint32_t(iy + cy) * rowOffset]));
                };
                V3 const h00 = at(0, 0);
                V3 const h10 = at(1, 0);
                V3 const h01 = at(0, 1);
                V3 const h11 = at(1, 1);
                float best = -1.0f;
                float const crossings[2] = { LiquidTriangle(local, localDir, h00, h10, h11),
                    LiquidTriangle(local, localDir, h00, h11, h01) };
                for (float t : crossings)
                    if (t >= 0.0f && t < distance && (best < 0.0f || t < best))
                        best = t;
                if (best >= 0.0f)
                {
                    distance = best;
                    return true;
                }
            }
            float const next = Min(nextX, nextY);
            if (next > tMax)
                break;
            if (nextX < nextY)
            {
                ix += stepX;
                nextX += deltaX;
            }
            else
            {
                iy += stepY;
                nextY += deltaY;
            }
        }
        return false;
    }

    /// The liquid a ray met: its LiquidType and whether that is magma or slime.
    struct LiquidFound
    {
        uint32_t Type;
        bool Deadly;
    };

    /// GroupModel::IntersectLiquid: the group's liquid, its crossing kept only within the group's bound.
    FORGE_HD inline bool GroupLiquid(uint32_t const* pool, uint32_t group, V3 origin, V3 dir, float& distance,
        LiquidFound& found)
    {
        uint32_t const* g = pool + group;
        if (!g[GROUP_LIQUID])
            return false;
        uint32_t const* liquid = pool + g[GROUP_LIQUID];
        float t = distance;
        if (!LiquidRay(liquid, origin, dir, t))
            return false;
        V3 const at = origin + dir * t;
        uint32_t const* bound = g + GROUP_BOUND;
        if (at.X < AsFloat(bound[0]) || at.X > AsFloat(bound[3]) || at.Y < AsFloat(bound[1])
            || at.Y > AsFloat(bound[4]))
            return false;
        distance = t;
        found.Type = liquid[LIQUID_TYPE];
        found.Deadly = liquid[LIQUID_DEADLY] != 0;
        return true;
    }

    /// WorldModel::IntersectLiquid.
    FORGE_HD inline bool ModelLiquid(uint32_t const* pool, uint32_t model, V3 origin, V3 dir, float& distance,
        LiquidFound& found, BihStack stack)
    {
        uint32_t const groups = pool[model + MODEL_GROUPS];
        if (groups == 1)
            return GroupLiquid(pool, GroupOffset(model, 0), origin, dir, distance, found);
        bool hit = false;
        LiquidFound mine = found;
        BihRayOn(stack, pool, model + MODEL_TREE, origin, dir, distance, [&](uint32_t entry, float& maxDist,
            BihStack)
        {
            if (GroupLiquid(pool, GroupOffset(model, entry), origin, dir, maxDist, mine))
                hit = true;
        });
        if (hit)
            found = mine;
        return hit;
    }

    /// ModelInstance::intersectLiquid: no model or an M2 has none.
    FORGE_HD inline bool InstanceLiquid(uint32_t const* instance, uint32_t const* pool, V3 origin, V3 dir,
        float& maxDist, LiquidFound& found, BihStack stack)
    {
        uint32_t const model = instance[INSTANCE_MODEL];
        if (!model || (instance[INSTANCE_FLAGS] & SPAWN_M2))
            return false;
        if (!RayMeetsBox(origin, dir, instance + INSTANCE_BOUND))
            return false;
        uint32_t const* invRot = instance + INSTANCE_INV_ROT;
        float const invScale = AsFloat(instance[INSTANCE_INV_SCALE]);
        V3 const p = Rotate(invRot, origin - ReadV3(instance + INSTANCE_POS)) * invScale;
        V3 const modelDir = Rotate(invRot, dir);
        float distance = maxDist * invScale;
        if (!ModelLiquid(pool, model, p, modelDir, distance, found, stack))
            return false;
        maxDist = distance * AsFloat(instance[INSTANCE_SCALE]);
        return true;
    }

    /// VMapMgr2::convertPositionToInternalRep: the static tree's space mirrors x and y about the map's middle.
    FORGE_HD inline V3 InternalRep(V3 p)
    {
        float const mid = 0.5f * 64 * 533.3333f;
        return Make(mid - p.X, mid - p.Y, p.Z);
    }

    /// A tree's first solid (Vision::SurfaceHit): the distance or < 0, and the hit's normal z facing the start.
    struct Surface
    {
        float Distance;
        float NormalZ;
    };

    /// MapCollisionData.cpp's FacingNormalZ.
    FORGE_HD inline float FacingNormalZ(V3 normal, V3 dir)
    {
        float const length = Length(normal);
        if (!(length > 1e-12f))
            return 0.0f;
        return (Dot(normal, dir) > 0.0f ? -normal.Z : normal.Z) / length;
    }

    /// StaticVMapCollisionData::GetSurfaceHit through StaticMapTree::GetSurfaceIntersection.
    FORGE_HD inline Surface StaticSurface(SceneView const& scene, V3 from, V3 to, BihStack stack)
    {
        Surface result = { -1.0f, 0.0f };
        if (!scene.Top)
            return result;
        V3 const pos1 = InternalRep(from);
        V3 const pos2 = InternalRep(to);
        float const length = Length(pos2 - pos1);
        if (!(length > 1e-6f) || !Finite(length))
            return result;
        V3 const dir = (pos2 - pos1) * (1.0f / length);
        float distance = length;
        V3 normal = Make(0.0f, 0.0f, 0.0f);
        bool hit = false;
        BihRayOn(stack, scene.Top, 0, pos1, dir, distance, [&](uint32_t entry, float& maxDist, BihStack free)
        {
            uint32_t const slot = scene.Slots[entry];
            if (slot == NO_INSTANCE)
                return;
            if (InstanceRay(scene.Instances + slot * INSTANCE_WORDS, scene.Models, pos1, dir, maxDist, normal,
                free))
                hit = true;
        });
        if (!hit)
            return result;
        result.Distance = distance;
        result.NormalZ = FacingNormalZ(normal, dir);
        return result;
    }

    /// StaticVMapCollisionData::GetLiquidHit through StaticMapTree::GetLiquidIntersection; Deadly as
    /// MapVisionWorld::ModelLiquid reads the type.
    FORGE_HD inline Surface StaticLiquid(SceneView const& scene, V3 from, V3 to, bool& deadly, BihStack stack)
    {
        Surface result = { -1.0f, 0.0f };
        if (!scene.Top)
            return result;
        V3 const pos1 = InternalRep(from);
        V3 const pos2 = InternalRep(to);
        float const length = Length(pos2 - pos1);
        if (!(length > 1e-6f) || !Finite(length))
            return result;
        V3 const dir = (pos2 - pos1) * (1.0f / length);
        float distance = length;
        LiquidFound found = { 0, false };
        bool hit = false;
        BihRayOn(stack, scene.LiquidTop, 0, pos1, dir, distance, [&](uint32_t entry, float& maxDist,
            BihStack free)
        {
            uint32_t const slot = scene.LiquidSlots[entry];
            if (slot != NO_INSTANCE
                && InstanceLiquid(scene.Instances + slot * INSTANCE_WORDS, scene.Models, pos1, dir, maxDist, found,
                free))
                hit = true;
        });
        if (!hit)
            return result;
        result.Distance = distance;
        deadly = found.Deadly;
        return result;
    }

    /// RegularGrid2D::Cell::ComputeCell, with its CELL_SIZE.
    FORGE_HD inline float DoorCellSize() { return float((533.33333f * 64.f) / float(DOOR_GRID)); }
    FORGE_HD inline void DoorCell(float x, float y, int32_t& cx, int32_t& cy)
    {
        cx = int32_t(x * (1.f / DoorCellSize()) + float(DOOR_GRID / 2));
        cy = int32_t(y * (1.f / DoorCellSize()) + float(DOOR_GRID / 2));
    }
    FORGE_HD inline bool DoorCellValid(int32_t cx, int32_t cy)
    {
        return cx >= 0 && cx < DOOR_GRID && cy >= 0 && cy < DOOR_GRID;
    }

    /// DynamicVMapCollisionData::GetSurfaceHit through DynamicMapTree::GetIntersectionTime: the dynamic tree's cell
    /// walk (RegularGrid2D::intersectRay, its border arithmetic as it is) and, in each cell it visits, the doors
    /// filed there (GameObjectModel::intersectRay: in the seat's phase, spawned).
    FORGE_HD inline Surface DynamicSurface(SceneView const& scene, uint32_t phaseMask, V3 from, V3 to,
        BihStack stack)
    {
        Surface result = { -1.0f, 0.0f };
        V3 const delta = to - from;
        float const length = Length(delta);
        if (!(length > 1e-6f) || !Finite(length))
            return result;
        if (!scene.DoorCount)
            return result;
        V3 const dir = delta * (1.0f / length);
        float maxDist = length;
        V3 normal = Make(0.0f, 0.0f, 0.0f);
        bool hit = false;
        auto const visitCell = [&](int32_t cx, int32_t cy)
        {
            uint32_t const cell = uint32_t(cx * DOOR_GRID + cy);
            uint32_t const offset = scene.DoorCells[cell * 2];
            uint32_t const count = scene.DoorCells[cell * 2 + 1];
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t const* door = scene.Doors + scene.DoorCells[offset + i] * INSTANCE_WORDS;
                if (!(door[INSTANCE_PHASE] & phaseMask) || !door[INSTANCE_SPAWNED])
                    continue;
                if (InstanceRay(door, scene.DoorModels, from, dir, maxDist, normal, stack))
                    hit = true;
            }
        };

        int32_t cellX = 0;
        int32_t cellY = 0;
        DoorCell(from.X, from.Y, cellX, cellY);
        if (DoorCellValid(cellX, cellY))
        {
            int32_t lastX = 0;
            int32_t lastY = 0;
            DoorCell(to.X, to.Y, lastX, lastY);
            if (cellX == lastX && cellY == lastY)
                visitCell(cellX, cellY);
            else
            {
                float const voxel = DoorCellSize();
                float const kxInv = 1.0f / dir.X;
                float const kyInv = 1.0f / dir.Y;
                float const bx = from.X;
                float const by = from.Y;
                int32_t stepX;
                int32_t stepY;
                float tMaxX;
                float tMaxY;
                if (kxInv >= 0)
                {
                    stepX = 1;
                    float const border = float(cellX + 1) * voxel;
                    tMaxX = (border - bx) * kxInv;
                }
                else
                {
                    stepX = -1;
                    float const border = float(cellX - 1) * voxel;
                    tMaxX = (border - bx) * kxInv;
                }
                if (kyInv >= 0)
                {
                    stepY = 1;
                    float const border = float(cellY + 1) * voxel;
                    tMaxY = (border - by) * kyInv;
                }
                else
                {
                    stepY = -1;
                    float const border = float(cellY - 1) * voxel;
                    tMaxY = (border - by) * kyInv;
                }
                float const tDeltaX = voxel * fabsf(kxInv);
                float const tDeltaY = voxel * fabsf(kyInv);
                do
                {
                    visitCell(cellX, cellY);
                    if (cellX == lastX && cellY == lastY)
                        break;
                    if (tMaxX < tMaxY)
                    {
                        tMaxX += tDeltaX;
                        cellX += stepX;
                    }
                    else
                    {
                        tMaxY += tDeltaY;
                        cellY += stepY;
                    }
                } while (DoorCellValid(cellX, cellY));
            }
        }
        if (!hit)
            return result;
        result.Distance = maxDist;
        result.NormalZ = FacingNormalZ(normal, dir);
        return result;
    }

    // ---- The camera caster, ported from Vision/VisionCaster.cpp ------------------------------------------------

    struct Hit
    {
        float Distance;
        uint32_t What;
        float Z;
        float NormalZ;
    };

    FORGE_HD inline float GridU(float x)
    {
        return float(Vision::GRID_CELLS) * (float(Vision::GRIDS / 2) - x / Vision::GRID_SIZE);
    }
    FORGE_HD inline float WorldOfU(float u)
    {
        return (float(Vision::GRIDS / 2) - u / float(Vision::GRID_CELLS)) * Vision::GRID_SIZE;
    }

    /// VisionCaster's Walk: the squares of side `size` the (u, v) path crosses between tStart and tEnd, in order;
    /// `visit(iu, iv, tIn, tOut)` returns false to stop.
    template <typename Visit>
    FORGE_HD inline void Walk(float u0, float v0, float du, float dv, float tStart, float tEnd, float size,
        int32_t lowU, int32_t lowV, int32_t highU, int32_t highV, Visit&& visit)
    {
        if (!(tEnd > tStart))
            return;
        float const inf = Inf();
        float const u = u0 + du * tStart;
        float const v = v0 + dv * tStart;
        int32_t iu = ClampI(int32_t(floorf(u / size)), lowU, highU);
        int32_t iv = ClampI(int32_t(floorf(v / size)), lowV, highV);
        int32_t const stepU = du > 0.0f ? 1 : -1;
        int32_t const stepV = dv > 0.0f ? 1 : -1;
        bool const flatU = fabsf(du) < 1e-12f;
        bool const flatV = fabsf(dv) < 1e-12f;
        float nextU = flatU ? inf : (float(iu + (du > 0.0f ? 1 : 0)) * size - u0) / du;
        float nextV = flatV ? inf : (float(iv + (dv > 0.0f ? 1 : 0)) * size - v0) / dv;
        float const deltaU = flatU ? inf : size / fabsf(du);
        float const deltaV = flatV ? inf : size / fabsf(dv);
        float t = tStart;
        while (true)
        {
            // std::max(t, std::min({ nextU, nextV, tEnd })): the first of the smallest.
            float least = nextU;
            if (nextV < least)
                least = nextV;
            if (tEnd < least)
                least = tEnd;
            float const next = Max(t, least);
            if (!visit(iu, iv, t, next) || next >= tEnd)
                return;
            t = next;
            if (nextU < nextV)
            {
                iu += stepU;
                nextU += deltaU;
            }
            else
            {
                iv += stepV;
                nextV += deltaV;
            }
            if (iu < lowU || iu > highU || iv < lowV || iv > highV)
                return;
        }
    }

    constexpr int32_t NO_LIMIT = 1 << 20;

    FORGE_HD inline bool OnMap(int32_t tileX, int32_t tileY)
    {
        return tileX >= 0 && tileY >= 0 && tileX < Vision::GRIDS && tileY < Vision::GRIDS;
    }

    FORGE_HD inline uint32_t const* GridOf(SceneView const& scene, int32_t tileX, int32_t tileY)
    {
        return scene.Grids[tileX * Vision::GRIDS + tileY];
    }

    /// Vision::Reach.
    FORGE_HD inline float Reach(V3 origin, V3 dir, SceneView const& scene)
    {
        float const du = -dir.X * float(Vision::GRID_CELLS) / Vision::GRID_SIZE;
        float const dv = -dir.Y * float(Vision::GRID_CELLS) / Vision::GRID_SIZE;
        float reach = Vision::REACH_MAX;
        Walk(GridU(origin.X), GridU(origin.Y), du, dv, 0.0f, Vision::REACH_MAX, float(Vision::GRID_CELLS),
            -NO_LIMIT, -NO_LIMIT, NO_LIMIT, NO_LIMIT, [&](int32_t tileX, int32_t tileY, float tIn, float)
            {
                if (OnMap(tileX, tileY) && GridOf(scene, tileX, tileY))
                    return true;
                reach = tIn;
                return false;
            });
        return reach;
    }

    /// VisionCaster's CellHeight (GridTerrainData::getHeight's rule).
    FORGE_HD inline float CellHeight(float const (&corner)[4], float centre, float fu, float fv)
    {
        float const h1 = corner[0];
        float const h2 = corner[1];
        float const h3 = corner[2];
        float const h4 = corner[3];
        float const h5 = 2.0f * centre;
        if (fu + fv < 1.0f)
        {
            if (fu > fv)
                return h1 + (h2 - h1) * fu + (h5 - h1 - h2) * fv;
            return h1 + (h5 - h1 - h3) * fu + (h3 - h1) * fv;
        }
        if (fu > fv)
            return (h2 + h4 - h5) * fu + (h4 - h2) * fv + (h5 - h4);
        return (h4 - h3) * fu + (h3 + h4 - h5) * fv + (h5 - h4);
    }

    /// VisionCaster's RayTriangleFromAbove (one-sided Moller-Trumbore).
    FORGE_HD inline float RayTriangleFromAbove(V3 origin, V3 dir, V3 a, V3 b, V3 c, float& normalZ)
    {
        V3 const e1 = b - a;
        V3 const e2 = c - a;
        V3 normal = Cross(e1, e2);
        if (normal.Z < 0.0f)
            normal = normal * -1.0f;
        if (Dot(normal, dir) >= 0.0f)
            return -1.0f;
        V3 const p = Cross(dir, e2);
        float const det = Dot(e1, p);
        if (fabsf(det) < 1e-12f)
            return -1.0f;
        float const inv = 1.0f / det;
        V3 const s = origin - a;
        float const u = Dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f)
            return -1.0f;
        V3 const q = Cross(s, e1);
        float const v = Dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f)
            return -1.0f;
        float const t = Dot(e2, q) * inv;
        if (t < 0.0f)
            return -1.0f;
        float const length = Length(normal);
        normalZ = length > 0.0f ? normal.Z / length : 1.0f;
        return t;
    }

    /// A cell's liquid plane is crossed within its footprint, give or take this much (VisionCaster's).
    constexpr float FOOTPRINT_SLACK = 1e-3f;

    /// Vision::CastTerrain, over the scene's terrain grids.
    FORGE_HD inline Hit CastTerrain(V3 origin, V3 dir, float limit, SceneView const& scene, bool liquids)
    {
        Hit best = { limit, uint32_t(Vision::Kind::Sky), origin.Z + dir.Z * limit, 0.0f };
        bool found = false;
        float const u0 = GridU(origin.X);
        float const v0 = GridU(origin.Y);
        float const du = -dir.X * float(Vision::GRID_CELLS) / Vision::GRID_SIZE;
        float const dv = -dir.Y * float(Vision::GRID_CELLS) / Vision::GRID_SIZE;

        Walk(u0, v0, du, dv, 0.0f, limit, float(Vision::GRID_CELLS), -NO_LIMIT, -NO_LIMIT, NO_LIMIT, NO_LIMIT,
            [&](int32_t tileX, int32_t tileY, float tileIn, float tileOut)
            {
                if (!OnMap(tileX, tileY))
                    return false;
                uint32_t const* grid = GridOf(scene, tileX, tileY);
                if (!grid)
                    return false;
                bool const terrain = grid != NoTerrain();
                bool const heights = terrain && grid[TERRAIN_HEIGHTS] != 0;
                float const maxHeight = terrain ? AsFloat(grid[TERRAIN_MAX]) : 0.0f;
                bool const tileLiquid = terrain && grid[TERRAIN_LIQUID] != 0;
                float const lowest = Min(origin.Z + dir.Z * tileIn, origin.Z + dir.Z * tileOut);
                bool const ground = heights && lowest <= maxHeight;
                bool const water = liquids && tileLiquid && dir.Z < 0.0f;
                if (!ground && !water)
                    return true;

                int32_t const lowU = tileX * Vision::GRID_CELLS;
                int32_t const lowV = tileY * Vision::GRID_CELLS;
                Walk(u0, v0, du, dv, tileIn, tileOut, 1.0f, lowU, lowV, lowU + Vision::GRID_CELLS - 1,
                    lowV + Vision::GRID_CELLS - 1, [&](int32_t u, int32_t v, float cellIn, float cellOut)
                    {
                        uint32_t const cx = uint32_t(u - lowU);
                        uint32_t const cy = uint32_t(v - lowV);
                        uint32_t const index = cx * TERRAIN_CELLS + cy;
                        uint32_t const flags = (grid[TERRAIN_FLAGS + index / 4] >> ((index % 4) * 8)) & 0xFFu;
                        bool const solid = (flags & CELL_SOLID) != 0;
                        float corner[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                        float centre = 0.0f;
                        if (solid)
                        {
                            uint32_t const* v9 = grid + TERRAIN_CORNERS;
                            corner[0] = AsFloat(v9[cx * TERRAIN_V9 + cy]);
                            corner[1] = AsFloat(v9[(cx + 1) * TERRAIN_V9 + cy]);
                            corner[2] = AsFloat(v9[cx * TERRAIN_V9 + cy + 1]);
                            corner[3] = AsFloat(v9[(cx + 1) * TERRAIN_V9 + cy + 1]);
                            centre = AsFloat(grid[TERRAIN_CENTRES + index]);
                        }
                        float nearest = -1.0f;
                        if (ground && solid)
                        {
                            auto const at = [&](float cu, float cv, float z)
                            {
                                return Make(WorldOfU(float(u) + cu), WorldOfU(float(v) + cv), z);
                            };
                            V3 const h1 = at(0.0f, 0.0f, corner[0]);
                            V3 const h2 = at(1.0f, 0.0f, corner[1]);
                            V3 const h3 = at(0.0f, 1.0f, corner[2]);
                            V3 const h4 = at(1.0f, 1.0f, corner[3]);
                            V3 const h5 = at(0.5f, 0.5f, centre);
                            V3 const triangles[4][3] = { { h1, h2, h5 }, { h1, h3, h5 }, { h2, h4, h5 },
                                { h3, h4, h5 } };
                            for (int i = 0; i < 4; ++i)
                            {
                                float normalZ = 1.0f;
                                float const t = RayTriangleFromAbove(origin, dir, triangles[i][0], triangles[i][1],
                                    triangles[i][2], normalZ);
                                if (t >= 0.0f && t <= limit && (nearest < 0.0f || t < nearest))
                                {
                                    nearest = t;
                                    best.What = uint32_t(Vision::Kind::Terrain);
                                    best.NormalZ = normalZ;
                                }
                            }
                        }
                        if (water && (flags & CELL_LIQUID))
                        {
                            float const level = AsFloat(grid[TERRAIN_LEVELS + index]);
                            float const t = (level - origin.Z) / dir.Z;
                            if (t >= 0.0f && t <= limit && t >= cellIn - FOOTPRINT_SLACK
                                && t <= cellOut + FOOTPRINT_SLACK && (nearest < 0.0f || t < nearest))
                            {
                                V3 const p = origin + dir * t;
                                float const fu = Clamp(GridU(p.X) - float(u), 0.0f, 1.0f);
                                float const fv = Clamp(GridU(p.Y) - float(v), 0.0f, 1.0f);
                                if (!solid || CellHeight(corner, centre, fu, fv) <= level)
                                {
                                    nearest = t;
                                    best.What = uint32_t((flags & CELL_DEADLY) ? Vision::Kind::Deadly
                                        : Vision::Kind::Water);
                                    best.NormalZ = 1.0f;
                                }
                            }
                        }
                        if (nearest < 0.0f)
                            return true;
                        best.Distance = nearest;
                        best.Z = origin.Z + dir.Z * nearest;
                        found = true;
                        return false;
                    });
                return !found;
            });

        if (!found)
        {
            best.Distance = limit;
            best.What = uint32_t(Vision::Kind::Sky);
            best.Z = origin.Z + dir.Z * limit;
            best.NormalZ = 0.0f;
        }
        return best;
    }

    /// Vision::RayCylinder.
    FORGE_HD inline float RayCylinder(V3 origin, V3 dir, float limit, DeviceUnit const& unit, bool& top)
    {
        top = false;
        float best = -1.0f;
        float const ox = origin.X - unit.X;
        float const oy = origin.Y - unit.Y;
        float const r2 = unit.Radius * unit.Radius;
        float const bottom = unit.Z;
        float const head = unit.Z + unit.Height;

        float const a = dir.X * dir.X + dir.Y * dir.Y;
        float const c = ox * ox + oy * oy - r2;
        if (a > 1e-9f && c > 0.0f)
        {
            float const b = 2.0f * (ox * dir.X + oy * dir.Y);
            float const discriminant = b * b - 4.0f * a * c;
            if (discriminant >= 0.0f)
            {
                float const t = (-b - sqrtf(discriminant)) / (2.0f * a);
                float const z = origin.Z + dir.Z * t;
                if (t >= 0.0f && t <= limit && z >= bottom && z <= head)
                    best = t;
            }
        }

        auto const cap = [&](float plane, bool isTop)
        {
            if (fabsf(dir.Z) < 1e-9f)
                return;
            float const t = (plane - origin.Z) / dir.Z;
            if (t < 0.0f || t > limit || (best >= 0.0f && t >= best))
                return;
            float const x = ox + dir.X * t;
            float const y = oy + dir.Y * t;
            if (x * x + y * y > r2)
                return;
            best = t;
            top = isTop;
        };
        if (origin.Z > head && dir.Z < 0.0f)
            cap(head, true);
        if (origin.Z < bottom && dir.Z > 0.0f)
            cap(bottom, false);
        return best;
    }

    /// VisionCaster's Nearest (liquids and units always: the pixels' cast; the boom stays on the CPU), on a stack
    /// of `Stack` entries; `overflow` is set when a walk dropped a node.
    template <int Stack>
    FORGE_HD inline Hit Nearest(V3 origin, V3 dir, float limit, SceneView const& scene, uint32_t phaseMask,
        DeviceUnit const* units, uint32_t unitCount, bool& overflow)
    {
        V3 const end = origin + dir * limit;
        Hit best = { limit, uint32_t(Vision::Kind::Sky), end.Z, 0.0f };
        BihStackNode nodes[Stack];
        BihStack const stack = { nodes, Stack, &overflow };

        // 1. The collision trees, the static and the doors apart.
        Surface const model = StaticSurface(scene, origin, end, stack);
        Surface const door = DynamicSurface(scene, phaseMask, origin, end, stack);
        bool const modelHit = model.Distance >= 0.0f && model.Distance <= limit;
        bool const doorHit = door.Distance >= 0.0f && door.Distance <= limit;
        if (modelHit || doorHit)
        {
            bool const isDoor = doorHit && (!modelHit || door.Distance < model.Distance);
            Surface const& hit = isDoor ? door : model;
            best.Distance = hit.Distance;
            best.What = uint32_t(isDoor ? Vision::Kind::Door : Vision::Kind::Model);
            best.Z = (origin + dir * best.Distance).Z;
            best.NormalZ = hit.NormalZ;
        }

        // 2. A WMO's liquid, entered from above.
        if (dir.Z < 0.0f)
        {
            bool deadly = false;
            Surface const liquid = StaticLiquid(scene, origin, origin + dir * best.Distance, deadly, stack);
            if (liquid.Distance >= 0.0f && liquid.Distance < best.Distance)
            {
                best.Distance = liquid.Distance;
                best.What = uint32_t(deadly ? Vision::Kind::Deadly : Vision::Kind::Water);
                best.Z = origin.Z + dir.Z * liquid.Distance;
                best.NormalZ = 1.0f;
            }
        }

        // 3. The terrain and its liquids, up to the nearest hit so far.
        Hit const terrain = CastTerrain(origin, dir, best.Distance, scene, true);
        if (terrain.What != uint32_t(Vision::Kind::Sky) && terrain.Distance < best.Distance)
            best = terrain;

        // 4. The units (the seat's own already left out).
        for (uint32_t i = 0; i < unitCount; ++i)
        {
            bool top = false;
            float const distance = RayCylinder(origin, dir, best.Distance, units[i], top);
            if (distance >= 0.0f && distance < best.Distance)
            {
                best.Distance = distance;
                best.What = uint32_t(units[i].Hostile ? Vision::Kind::Hostile : Vision::Kind::Other);
                best.Z = origin.Z + dir.Z * distance;
                best.NormalZ = top ? 1.0f : 0.0f;
            }
        }
        return best;
    }

    /// Vision::ObjectiveFlag.
    FORGE_HD inline bool ObjectiveFlag(V3 origin, V3 dir, float distance, FrameRequest const& request)
    {
        if (!request.HasObjective)
            return false;
        V3 const toward = Make(request.ObjectiveX, request.ObjectiveY, request.ObjectiveZ) - origin;
        float const along = Clamp(Dot(toward, dir), 0.0f, Max(0.0f, distance));
        return Length(toward - dir * along) <= Vision::OBJECTIVE_RADIUS;
    }

    /// Vision::EncodePixel.
    FORGE_HD inline void EncodePixel(Hit const& hit, float feetZ, bool objective, uint8_t* out)
    {
        bool const sky = hit.What == uint32_t(Vision::Kind::Sky);
        float const distance = Clamp(logf(Max(hit.Distance, Vision::NEAR) / Vision::NEAR)
            / logf(Vision::DISTANCE_REFERENCE / Vision::NEAR), 0.0f, 1.0f);
        out[0] = sky ? Vision::SKY_BYTE : uint8_t(lroundf(Vision::DISTANCE_LEVELS * distance));
        long const rounded = lroundf((hit.Z - feetZ) / Vision::HEIGHT_STEP);
        int32_t const steps = rounded < -Vision::HEIGHT_LIMIT ? -Vision::HEIGHT_LIMIT
            : (rounded > Vision::HEIGHT_LIMIT ? Vision::HEIGHT_LIMIT : int32_t(rounded));
        out[1] = sky ? Vision::HEIGHT_ZERO : uint8_t(int32_t(Vision::HEIGHT_ZERO) + steps);
        out[2] = uint8_t(lroundf(255.0f * Clamp(hit.NormalZ, 0.0f, 1.0f)));
        out[3] = uint8_t((uint8_t(hit.What) & Vision::KIND_MASK) | (objective ? Vision::OBJECTIVE_BIT : 0));
    }

    /// Vision::PixelDirection: Camera.h's Direction at the pixel's angles. cos and sin go through double, so the
    /// float they round to is the correctly rounded one, which is what glibc's cosf and sinf return nearly always.
    FORGE_HD inline V3 PixelDirection(FrameRequest const& request, uint32_t row, uint32_t col)
    {
        float const yawRight = ((float(col) + 0.5f) / float(request.CastWidth) * request.FovH - request.FovH / 2.0f)
            * Vision::DEGREES;
        float const pitchUp = (request.FovV / 2.0f - (float(row) + 0.5f) / float(request.CastHeight) * request.FovV)
            * Vision::DEGREES;
        float const azimuth = request.Azimuth - yawRight;
        float const elevation = request.Elevation + pitchUp;
        float const cosElevation = float(cos(double(elevation)));
        return Make(cosElevation * float(cos(double(azimuth))), cosElevation * float(sin(double(azimuth))),
            float(sin(double(elevation))));
    }

    /// One cast pixel, written to `out` (4 bytes): Vision::Render's loop body (CastRay, ObjectiveFlag, EncodePixel),
    /// on a stack of `Stack` entries. True when a walk overflowed it (the pixel may then differ from the CPU's).
    template <int Stack>
    FORGE_HD inline bool CastPixel(FrameRequest const& request, SceneView const& scene, DeviceUnit const* units,
        uint32_t row, uint32_t col, uint8_t* out)
    {
        V3 const camera = Make(request.CameraX, request.CameraY, request.CameraZ);
        V3 const dir = PixelDirection(request, row, col);
        float const reach = Reach(camera, dir, scene);
        bool overflow = false;
        Hit const hit = Nearest<Stack>(camera, dir, reach, scene, request.PhaseMask, units + request.UnitOffset,
            request.UnitCount, overflow);
        bool const flag = ObjectiveFlag(camera, dir, hit.Distance, request);
        EncodePixel(hit, request.FeetZ, flag, out);
        return overflow;
    }

    /// CastPixel at STACK_SIZES[index], chosen at run time (the host's; a kernel is compiled at each size).
    inline bool CastPixelSized(int index, FrameRequest const& request, SceneView const& scene,
        DeviceUnit const* units, uint32_t row, uint32_t col, uint8_t* out)
    {
        static_assert(STACK_COUNT == 4, "one case a stack size");
        switch (index)
        {
            case 0:
                return CastPixel<STACK_SIZES[0]>(request, scene, units, row, col, out);
            case 1:
                return CastPixel<STACK_SIZES[1]>(request, scene, units, row, col, out);
            case 2:
                return CastPixel<STACK_SIZES[2]>(request, scene, units, row, col, out);
            default:
                return CastPixel<STACK_SIZES[3]>(request, scene, units, row, col, out);
        }
    }

    /// Whether the frame is cast at a size other than the canonical one (and so into scratch, then scaled up).
    FORGE_HD inline bool Scaled(FrameRequest const& request)
    {
        return request.CastWidth != request.Width || request.CastHeight != request.Height;
    }

    /// Camera.h's Upscale, for one canonical pixel.
    FORGE_HD inline void UpscalePixel(uint8_t const* from, uint32_t w, uint32_t h, uint8_t* to, uint32_t W,
        uint32_t H, uint32_t r, uint32_t c)
    {
        uint32_t const sr = uint32_t(uint64_t(r) * h / H);
        uint32_t const sc = uint32_t(uint64_t(c) * w / W);
        uint8_t const* in = from + (uint64_t(sr) * w + sc) * Vision::BYTES_PER_PIXEL;
        uint8_t* out = to + (uint64_t(r) * W + c) * Vision::BYTES_PER_PIXEL;
        for (uint32_t b = 0; b < Vision::BYTES_PER_PIXEL; ++b)
            out[b] = in[b];
    }
}

/// The launch the device library takes (DeviceApi.h's CastVision): device pointers to the requests, the units, the
/// scenes and the two byte buffers. Plain C, like the rest of that boundary.
extern "C"
{
    struct ForgeVisionLaunch
    {
        Animus::GpuVision::FrameRequest const* Requests;
        uint32_t RequestCount;
        /// The largest cast frame's pixel count and the largest canonical one's: the grids' widths.
        uint32_t MaxCastPixels;
        uint32_t MaxPixels;
        /// The widest and the tallest cast frame (the cast grid's tiles cover both).
        uint32_t MaxCastWidth;
        uint32_t MaxCastHeight;
        Animus::GpuVision::DeviceUnit const* Units;
        Animus::GpuVision::SceneView const* Scenes;
        uint8_t* Scratch;
        uint8_t* Image;
        /// The stack the launch's scenes can need at worst (STACK_SIZES picks the kernel), and a device counter
        /// the kernel adds every overflowing pixel to (zeroed by the caller).
        uint32_t StackDepth;
        uint32_t Pad;
        uint32_t* Overflows;
    };
}

#endif
