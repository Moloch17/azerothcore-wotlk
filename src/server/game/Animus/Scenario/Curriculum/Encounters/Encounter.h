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

#ifndef ANIMUS_LIB_CURRICULUM_ENCOUNTER_H
#define ANIMUS_LIB_CURRICULUM_ENCOUNTER_H

#include "Define.h"
#include "RewardLedger.h"

class Battleground;
#include <vector>

class Map;
class Player;
class Unit;

namespace Animus
{
    struct Env;
}

namespace Animus::Curriculum
{
    class StageScenario;
    class EpisodeInfoTable;
    struct SeatActionResult;
    struct SeatView;
    namespace DirectorLayout { struct DirectorView; }

    /// Who stood up again after a pull (see Encounter::OnRecovered).
    constexpr int32 RECOVERED_OWNER = -1;

    /// One part of what a stage's envs contain besides the seats: a creature, pulls, the owner, the party group, the
    /// enemy player. The scenario creates the encounters any of its arenas asks for; each episode it calls the hooks
    /// of the encounters the episode's arena uses, in a fixed order. An encounter keeps its own per-env state and adds
    /// its own rewards, episode info and critic state. Episode info columns and reward terms are the union over the
    /// stage's arenas: an encounter an episode does not use reports 0.
    class Encounter
    {
    public:
        explicit Encounter(StageScenario& scenario) : _scenario(scenario) { }
        virtual ~Encounter() = default;

        Encounter(Encounter const&) = delete;
        Encounter& operator=(Encounter const&) = delete;

        /// The reward terms the encounter pays (each gets a reward_<name> episode info column).
        [[nodiscard]] virtual std::vector<RewardTerm> RewardTerms() const { return {}; }

        /// Once, at construction: the encounter's episode info columns.
        virtual void AddEpisodeInfo(EpisodeInfoTable& /*table*/) { }

        /// A new episode starts: clear the encounter's episode totals.
        virtual void ResetEpisode(Env& /*env*/) { }

        /// Before the seats' old bots are replaced.
        virtual void BeforeRebuild(Env& /*env*/) { }
        /// After the episode's arena and its seats' classes are drawn and before its level: an encounter that fixes
        /// the level, the map or the spawn (an instance's boss rung) writes them into the EnvState here.
        virtual void BeforeLevel(Env& /*env*/) { }
        /// After the episode's level is drawn and before its seats are built. A battleground has to exist before
        /// them: a seat reaches its map through the battleground instance it was told to join.
        virtual void BeforeSeats(Env& /*env*/, uint8 /*level*/) { }
        /// The battleground this encounter runs for `env`, when it runs one: its seats join it rather than being
        /// placed in an instance of their own.
        [[nodiscard]] virtual Battleground* MatchFor(Env const& /*env*/) const { return nullptr; }

        /// After the seats' new bots are placed, in encounter order: build what the episode fights and prepare the
        /// seats for it. `level` is the seats' level. False when the env cannot be built.
        virtual bool Build(Env& /*env*/, Map* /*map*/, uint8 /*level*/) { return true; }

        /// Each decision, before the actions: enemies joining fights, then everything else.
        virtual void UpdateEnemies(Env& /*env*/) { }
        virtual void Update(Env& /*env*/) { }

        /// What seat `seat`'s actions aim at, when this encounter decides it. True if it did (target may be null).
        virtual bool SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& /*target*/) { return false; }

        /// Before a seat's action is applied, and after, with what it did.
        virtual void BeforeSeatAction(Env& /*env*/, uint32 /*seat*/, Unit* /*target*/) { }
        virtual void OnSeatAction(Env& /*env*/, uint32 /*seat*/, SeatActionResult const& /*result*/) { }

        /// Fill the parts of a seat's view this encounter knows.
        virtual void View(Env const& /*env*/, uint32 /*seat*/, SeatView& /*view*/) const { }

        /// Fill the parts of a side's director view this encounter knows -- the objective, mostly: a flag match
        /// knows the score and who carries what, and nothing else does.
        virtual void ViewDirector(Env const& /*env*/, uint32 /*side*/,
            DirectorLayout::DirectorView& /*view*/) const { }

        /// Each decision: before the seats are rewarded, each seat's reward terms, after every seat was rewarded.
        virtual void BeforeRewards(Env& /*env*/) { }
        virtual void Reward(Env& /*env*/, uint32 /*seat*/, Player* /*bot*/, RewardLedger& /*ledger*/) { }
        virtual void AfterRewards(Env& /*env*/) { }

        /// Write the encounter's part of the critic state (the whole buffer, already zeroed).
        virtual void WriteState(Env const& /*env*/, float* /*state*/) const { }

        [[nodiscard]] virtual bool IsTerminal(Env const& /*env*/) const { return false; }

        /// Seat `seat` (or RECOVERED_OWNER) stood up again after a pull: its death can be paid for again.
        virtual void OnRecovered(Env& /*env*/, int32 /*who*/) { }

        /// A new pull is about to spawn (the pulls encounter announces it; see StageScenario::NotifyPullStarting).
        virtual void OnPullStarting(Env& /*env*/) { }

        /// The env's next episode is an arena without this encounter: remove what it keeps in the world (a bot, a
        /// group), before the seats are rebuilt. It may be built again for a later episode.
        virtual void Deactivate(Env& env) { Teardown(env); }

        /// Once at shutdown: remove what the encounter spawned.
        virtual void Teardown(Env& /*env*/) { }

    protected:
        StageScenario& _scenario;
    };
}

#endif
