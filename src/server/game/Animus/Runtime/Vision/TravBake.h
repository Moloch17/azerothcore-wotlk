/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU Affero
 * General Public License as published by the Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef ANIMUS_RUNTIME_VISION_TRAV_BAKE_H
#define ANIMUS_RUNTIME_VISION_TRAV_BAKE_H

#include <cstdint>
#include <string>
#include <vector>

class Map;

namespace Animus::Vision
{
    /// **The traversability bake** (vision movement curriculum, decision 0036; CONTRACT.md sec 1): a 2-yd grid over
    /// an arena box classified by the player controller's own rules -- what a player's body can walk, step up, jump,
    /// drop and swim -- with 8-neighbour edge costs in yards. Training labels and the route cost-to-go the learner's
    /// potential and planner read; never an input to the policy and never from the navmesh. Baked at an arena's first
    /// episode and cached to disk beside the camera's scenes (`trav/`).
    enum class TravClass : uint8_t
    {
        Unknown = 0,    // outside the bake, or no ground found
        Free = 1,       // ground a body walks
        Wall = 2,       // no passable edge into it (a blocked line of sight from every neighbour)
        Water = 3,      // liquid deeper than 1.5 yd: swum
        Hazard = 4,     // magma or slime
        StepUp = 5,     // reached from a neighbour by a step (within STEP_UP)
        Jump = 6,       // reached only by a jump (STEP_UP < rise <= JUMP_RISE)
        DropSafe = 7,   // a drop under MIN_FALL_DMG_DIST starts here
        DropHurt = 8,   // a damaging drop starts here
        DropLethal = 9, // a lethal drop (damageperc >= 1) starts here: no edge
        Slope = 10,     // too steep to walk (normal z under WALKABLE_NORMAL_Z)
        Count
    };
    constexpr uint32_t TRAV_CLASSES = uint32_t(TravClass::Count);
    constexpr char const* TRAV_CLASS_NAMES[TRAV_CLASSES] = { "unknown", "free", "wall", "water", "hazard", "step_up",
        "jump", "drop_safe", "drop_hurt", "drop_lethal", "slope" };

    constexpr float TRAV_CELL = 2.0f;
    constexpr float TRAV_UNREACHABLE = 65504.0f;   // the largest f16: no route
    /// Player::HandleFall: the first damaging drop, the damage line (share of maximum health per yard and its
    /// intercept) and the drop that kills from full health.
    constexpr float FALL_DAMAGE_FROM = 13.48f;
    constexpr float FALL_DAMAGE_SLOPE = 0.018f;
    constexpr float FALL_DAMAGE_INTERCEPT = -0.2426f;
    constexpr float FALL_LETHAL_FROM = (1.0f - FALL_DAMAGE_INTERCEPT) / FALL_DAMAGE_SLOPE;    // 69.03 yd
    /// A standing jump's rise (PlayerController JUMP_SPEED 7.9555 at gravity 19.29: 1.64 yd).
    constexpr float JUMP_RISE = 1.64f;

    // The sim core agent fills in the grid, the baker and Dijkstra here (TravBake.cpp), per CONTRACT.md sec 1:
    //   struct TravGrid { box, columns, rows, classes[], ground z[], liquid level[], edges } ;
    //   bool EnsureBake(Map* map, box, dir, TravGrid&, error);  (bake or load; versioned, digested)
    //   void CostToGo(TravGrid const&, goal cell, std::vector<float>& c);   (Dijkstra, yards)
}

#endif
