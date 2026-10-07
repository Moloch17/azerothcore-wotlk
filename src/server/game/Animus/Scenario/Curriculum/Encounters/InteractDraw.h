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

#ifndef ANIMUS_LIB_CURRICULUM_INTERACT_DRAW_H
#define ANIMUS_LIB_CURRICULUM_INTERACT_DRAW_H

#include "EntityActions.h"
#include "SeekDraw.h"
#include "StageDefinition.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

/// **The interact stage's draws and judgements** (InteractEncounter, M3), as pure functions of the site table, the
/// object pool and uniform numbers in [0, 1), so they are tested with fixed numbers (InteractStageTest) and the
/// encounter only supplies the randomness and the world.
namespace Animus::Curriculum::InteractDraw
{
    /// **The ladder** (dungeon-curriculum M3): what the episode asks, in the order the difficulty comes in --
    /// - Distinguish: the named object among decoys of other kinds, all in sight of the seat: reach the right one;
    /// - Switch: the named object behind a shut door; the door's lever, on the seat's side, opens it;
    /// - Key: the lock the goal names (the cannon), which only the key item the seat carries opens: use it on it.
    /// The rung follows the shaping fade's (Rung of the scale: FADE_SCALES, configs/move3_interact.yaml fade.rungs), so
    /// it steps on the fade's gate alone, and every rung keeps CarryShare of the one below.
    enum class Rung : uint8
    {
        Distinguish = 0,
        Switch = 1,
        Key = 2,
        Count
    };
    constexpr uint32 RUNGS = uint32(Rung::Count);
    constexpr std::array<char const*, RUNGS> RUNG_NAMES = { "distinguish", "switch", "key" };
    /// The fade's scales the rungs sit on (configs/move3_interact.yaml fade.rungs).
    constexpr std::array<float, RUNGS> FADE_SCALES = { 1.0f, 0.5f, 0.0f };

    /// What the goal asks done with the object it names (SeatView::NamedTask; the sight block's named row).
    enum class Task : uint8
    {
        None = 0,
        Reach = 1,
        Use = 2,
        UseItem = 3
    };

    /// The rung of shaping scale `shaping`: the nearest of FADE_SCALES.
    inline Rung RungOf(float shaping)
    {
        uint32 best = 0;
        for (uint32 rung = 1; rung < RUNGS; ++rung)
            if (std::fabs(FADE_SCALES[rung] - shaping) < std::fabs(FADE_SCALES[best] - shaping))
                best = rung;
        return Rung(best);
    }

    /// The rung a training episode plays: the ladder's, or with probability `carry` (`u` below it) the one below.
    inline Rung PlacedRung(Rung ladder, float u, float carry)
    {
        return ladder != Rung::Distinguish && u < carry ? Rung(uint8(ladder) - 1) : ladder;
    }

    /// The task a rung asks: reach the named object, or use the key item on the named lock.
    inline Task TaskOf(Rung rung)
    {
        return rung == Rung::Key ? Task::UseItem : Task::Reach;
    }

    /// The sites a rung plays at, in table order: every site (distinguish), those a lever opens (switch), those a
    /// key item opens (key).
    inline std::vector<uint32> RungSites(std::vector<InteractSite> const& sites, Rung rung)
    {
        std::vector<uint32> out;
        for (uint32 index = 0; index < sites.size(); ++index)
            if (rung == Rung::Distinguish || (rung == Rung::Switch) == (sites[index].Key == 0))
                out.push_back(index);
        return out;
    }

    /// The index `u` falls on among `count` (0 for none).
    inline uint32 Index(float u, uint32 count)
    {
        return count ? std::min<uint32>(count - 1, uint32(std::clamp(u, 0.0f, 1.0f) * float(count))) : 0;
    }

    /// How many decoys: DecoysMin to DecoysMax by `u`, never more than the pool's other kinds.
    inline uint32 DecoyCount(float u, uint32 least, uint32 most, uint32 objects)
    {
        uint32 const others = objects ? objects - 1 : 0;
        least = std::min(least, others);
        most = std::clamp(most, least, others);
        return least + Index(u, most - least + 1);
    }

    /// The decoys' kinds: `count` of the pool's other kinds than `target`, the pool's order turned by `u` (each kind
    /// once: a decoy is never the named object's kind).
    inline std::vector<uint32> DecoyKinds(uint32 target, uint32 count, uint32 objects, float u)
    {
        std::vector<uint32> others;
        for (uint32 kind = 0; kind < objects; ++kind)
            if (kind != target)
                others.push_back(kind);
        std::vector<uint32> out;
        uint32 const start = Index(u, uint32(others.size()));
        for (uint32 i = 0; i < count && i < others.size(); ++i)
            out.push_back(others[(start + i) % others.size()]);
        return out;
    }

    /// An evaluation's episode for seed `seed` at rung `rung`: its site (the rung's sites in turn), its spawn and its
    /// spots and kinds, each from SeekDraw::SeedUniform of the seed, so every checkpoint meets the same layouts.
    struct EvaluationEpisode
    {
        uint32 Site = 0;
        float SpawnU = 0.0f;
        float TargetU = 0.0f;
        float KindsU = 0.0f;
        float CountU = 0.0f;
        float FacingU = 0.0f;
    };
    inline EvaluationEpisode EvaluationPick(uint32 seed, std::vector<uint32> const& sites)
    {
        EvaluationEpisode out;
        out.Site = sites.empty() ? 0 : sites[seed % sites.size()];
        out.SpawnU = SeekDraw::SeedUniform(seed, 101);
        out.TargetU = SeekDraw::SeedUniform(seed, 102);
        out.KindsU = SeekDraw::SeedUniform(seed, 104);
        out.CountU = SeekDraw::SeedUniform(seed, 105);
        out.FacingU = SeekDraw::SeedUniform(seed, 106);
        return out;
    }

    /// The held-out sweep's rung for seed `seed`: every rung in turn, so the sweep (and its videos) covers all three.
    inline Rung SweepRung(uint32 seed)
    {
        return Rung(seed % RUNGS);
    }

    /// **The site's door over an episode** (DoorOpened): watched once a decision. It is paid once an episode, on the
    /// door's first opening (shut -> open) after the seat's own press on its lever; pressing the lever again, or the
    /// door shutting and opening again, pays nothing more. An opening before any press (none the seat made) is measured,
    /// never paid, and a later press's opening still pays.
    struct DoorWatch
    {
        bool Pressed = false;       // the seat has pressed the site's lever this episode
        bool WasOpen = false;       // the door at the last look
        bool Opened = false;        // it has opened at all (door_opened)
        bool ByLever = false;       // ... after the seat's own press (door_by_lever)
        bool Paid = false;

        /// The lever pressed (sent) this decision.
        void Press() { Pressed = true; }
        /// The door as it stands now; true when this look pays DoorOpened.
        bool Look(bool open)
        {
            bool const opening = open && !WasOpen;
            WasOpen = open;
            if (!opening)
                return false;
            Opened = true;
            if (!Pressed)
                return false;
            ByLever = true;
            if (Paid)
                return false;
            Paid = true;
            return true;
        }
    };

    /// Whether a press reached the object it named: sent, or refused for what the object is (it would loot, it does
    /// not take that press, it is locked, no key fits it) rather than for where it is (out of reach, gone). A decoy
    /// reached so is taken for the named object (WrongObject); passing near one is not.
    inline bool Reached(uint8 refusal)
    {
        using EntityActions::Refusal;
        switch (Refusal(refusal))
        {
            case Refusal::None:
            case Refusal::Loot:
            case Refusal::Kind:
            case Refusal::Locked:
            case Refusal::NoItem:
                return true;
            default:
                return false;
        }
    }

    /// **What a sight press came to** (OnSeatAction, from SeatActionResult): the press sent (not refused) on the
    /// named object -- reached by a use, or its lock opened by the key -- on a decoy, on the site's opener, or on
    /// anything else (nothing).
    enum class Verdict : uint8
    {
        None,
        Right,          // the named object used (Task::Reach and Use), or its lock given the key (Task::UseItem)
        Wrong,          // a decoy pressed
        Opener,         // the site's lever pressed
    };
    struct PressFacts
    {
        uint8 Press = 0;                    // EntityActions::Press
        bool Sent = false;                  // not refused
        bool Reached = false;               // sent, or refused for what the object is (InteractDraw::Reached)
        uint64 On = 0;                      // the entity pressed (raw GUID)
        uint64 Target = 0;                  // the named object
        uint64 Opener = 0;                  // the site's lever or lock
        Task Asked = Task::Reach;
        std::vector<uint64> const* Decoys = nullptr;
    };
    inline Verdict Judge(PressFacts const& facts)
    {
        using EntityActions::Press;
        bool const acts = facts.Press == uint8(Press::Interact) || facts.Press == uint8(Press::UseItem);
        if (!facts.On || !acts)
            return Verdict::None;
        // A decoy pressed within reach is taken for the named object, whether or not the world took the press.
        if ((facts.Sent || facts.Reached) && facts.On != facts.Target && facts.Decoys
            && std::find(facts.Decoys->begin(), facts.Decoys->end(), facts.On) != facts.Decoys->end())
            return Verdict::Wrong;
        if (!facts.Sent)
            return Verdict::None;
        if (facts.On == facts.Target)
        {
            // A lock is opened by its key, not a hand on it (a hand on it is refused before it is sent: Locked).
            if (facts.Asked == Task::UseItem)
                return facts.Press == uint8(Press::UseItem) ? Verdict::Right : Verdict::None;
            return Verdict::Right;
        }
        if (facts.On == facts.Opener && facts.Press == uint8(Press::Interact))
            return Verdict::Opener;
        return Verdict::None;
    }
}

#endif
