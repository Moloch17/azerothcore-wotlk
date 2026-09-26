/*
 * This file is part of the Animus project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_STAGE_SETTINGS_H
#define ANIMUS_LIB_STAGE_SETTINGS_H

#include "Define.h"
#include "Position.h"
#include <string>
#include <vector>

namespace Animus
{
    /// What a host (the forge's trainer, mod-animus's stage viewer) tells a scenario and its env pool: how many envs,
    /// where they are, how often they decide and which settings keys tune the curriculum. The host reads them from its
    /// own config.
    struct StageSettings
    {
        /// Envs of the pool; each is its own instance map unless the host places it (Env::MapId/InstanceId).
        uint32 Envs = 1;

        /// Env ids run from here (Env::Id): bot account ids and names derive from them, so pools running side by side
        /// in one server need disjoint ranges.
        uint32 FirstEnvId = 0;

        /// Game time per decision: per-decision reward terms are tuned per 50 ms and scaled by it.
        uint32 DecisionMs = 100;

        /// Episode length for arenas without their own.
        uint32 EpisodeSeconds = 60;

        /// Episode info means are kept every this many finished episodes (EnvPool::LastEpisodeMeans).
        uint32 ReportEpisodes = 256;
        /// AnimusForge.ResetOnMapThreads: a scenario whose resets stay on their map (Scenario::ResetsStayOnMap)
        /// rebuilds each ended episode on the thread updating its map, not afterwards on the world thread.
        bool ResetOnMapThreads = true;

        /// The classes the seats play (warrior, druid, ...); empty = every class. A class brings every role it
        /// can play with it -- there is one model per class, not per class and role.
        std::vector<std::string> Classes;

        /// The instanceable map and position every env's seats start at.
        uint32 SpawnMapId = 560;
        Position SpawnPosition{ 2741.9f, 1315.2f, 14.0f, 2.96f };

        /// Every character's level, or 0 for the curriculum's random levels.
        uint32 Level = 0;

        /// How many Map objects a continent stage spreads its envs over (MapMgr::CreateContinentReplica), so a
        /// continent is not one map and therefore one map task for the whole pool. 0 asks for the fewest the
        /// phase cap allows: an env stands in one of the 31 phase bits its map has, so 31 envs per replica.
        /// Sizing, like Envs or MapUpdate.Threads. A replica shares the continent's terrain, collision tree
        /// and navmesh with the base map and loads grids only where its envs stand, so it costs the creatures
        /// and gameobjects of those grids rather than a copy of the continent. At or below 31 envs the default
        /// is one map, which is what a continent stage has always been.
        uint32 ContinentReplicas = 0;

        /// Prefix of the curriculum tuning keys (CurriculumTuning::Load) and arena weights, with the trailing dot:
        /// "AnimusForge.Curriculum." for the forge, "Animus.Curriculum." for mod-animus.
        std::string TuningPrefix;

        /// Where a stage writes its layout manifests and stage.json (<LayoutsDir>/<stage>/); empty = nowhere.
        std::string LayoutsDir;
    };
}

#endif
