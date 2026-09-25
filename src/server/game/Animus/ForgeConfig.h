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

#ifndef MOD_ANIMUS_FORGE_CONFIG_H
#define MOD_ANIMUS_FORGE_CONFIG_H

#include "Define.h"
#include "Position.h"
#include "StageSettings.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace AnimusForge
{
    /// Module settings, read once at startup (mod_animus_forge.conf.dist documents every key). Only settings: what is
    /// running lives in Forge. The curriculum's tuning is CurriculumTuning.
    struct ForgeConfig
    {
        bool Enable = true;

        /// AnimusForge.Queue: the scenarios `forge start` trains, one after another, when given none. Empty =
        /// every curriculum stage, first to last.
        std::vector<std::string> Queue;

        /// AnimusForge.Queue.SkipFinished: `forge start` without scenarios skips those whose run already finished
        /// and moved on (<RunsDir>/<name>/finished.json with "advanced": true).
        bool QueueSkipFinished = true;

        /// AnimusForge.Queue.LocalEpisodes: with a local policy, episodes per scenario of `forge start` (0 = run
        /// the first one until cancelled).
        uint32 QueueLocalEpisodes = 0;

        uint32 Envs = 64;
        /// AnimusForge.ContinentReplicas: Map objects a continent stage spreads its envs over, so the continent
        /// is not one map task for the whole pool. 0 = the fewest the 31 phase bits allow (31 envs each).
        uint32 ContinentReplicas = 0;
        /// AnimusForge.Stage.<name>.Envs: a stage's own env count where the default would not do (forty seats an
        /// env at 128 envs is 5,120 bots), so `forge start stage32_raid40` needs no conf edit.
        std::map<std::string, uint32> StageEnvs;
        /// AnimusForge.DecisionMs: game time per decision. Everything that scales a reward or measures elapsed game
        /// time is in these units, and it is what the learner is told the step is worth.
        uint32 DecisionMs = 250;
        /// AnimusForge.TicksPerDecision: world updates per decision. At 1 (the default) a tick is a decision, which is
        /// what the sim has always done. Above 1 the forge core ticks at DecisionMs / TicksPerDecision -- ForgeMain's
        /// ForgeUpdateLoop reads both keys -- so splines, auras and the fight run at the finer step while the policy
        /// still chooses every DecisionMs. It buys smooth movement without paying for more decisions.
        uint32 TicksPerDecision = 1;
        /// DecisionMs / TicksPerDecision: how far a map moves per tick.
        [[nodiscard]] uint32 TickMs() const
        {
            return std::max<uint32>(1, DecisionMs / std::max<uint32>(1, TicksPerDecision));
        }
        /// AnimusForge.HalfBatch: the pool in two halves whose maps tick in turn, so the learner decides one half
        /// while the other's maps tick (see AnimusForge::Forge::IsMapFrozen). Needs TicksPerDecision 1.
        bool HalfBatch = false;
        [[nodiscard]] bool HalvesTick() const { return HalfBatch && TicksPerDecision == 1; }
        /// The world tick, and what the module expects OnUpdate's diff to be: TickMs, or half of it in half-batch,
        /// where each half's maps tick every other world tick with the time of both.
        [[nodiscard]] uint32 WorldTickMs() const
        {
            return HalvesTick() ? std::max<uint32>(1, TickMs() / 2) : TickMs();
        }
        uint32 EpisodeSeconds = 60;

        std::string Policy;
        uint32 ReportEpisodes = 256;
        std::string SocketPath;

        /// AnimusForge.OutputDir, resolved: where runs/ and layouts/ go. Never empty after Load.
        std::string OutputDir;

        /// Remote policy only: start the Python learner as a child process once the socket is up.
        bool LearnerAutoStart = true;
        std::string LearnerPython;      // resolved: never empty after Load
        std::string LearnerWorkDir;     // resolved: never empty after Load
        std::string LearnerConfig;      // AnimusForge.Learner.Config; empty = configs/<scenario>.yaml
        std::string LearnerLogFile;     // resolved: never empty after Load
        std::vector<std::string> LearnerArgs;   // AnimusForge.Learner.Args, split on whitespace
        /// AnimusForge.Learner.TorchThreads: CPU threads the learner's torch uses (0 = torch's own default). The
        /// learner runs beside the map update threads on the same cores, so `forge bench` sweeps both.
        uint32 LearnerTorchThreads = 0;
        /// AnimusForge.Learner.Device: the device the learner updates and rolls out on ("cuda:1" for a second GPU;
        /// empty = the stage config's train_device and rollout_device). A GPU the machine does not have falls back
        /// to the first one, so the key can name the machine as it will be.
        std::string LearnerDevice;
        std::vector<std::string> Classes;       // AnimusForge.Classes; empty = every class

        /// AnimusForge.SpawnPoint.*: the instanceable map and position every env's bots start at.
        uint32 SpawnMapId = 560;
        Position SpawnPosition;

        /// AnimusForge.ModelDir, resolved: where `forge export` writes models. Never empty after Load.
        std::string ModelDir;
        /// AnimusForge.Progress.Interval, seconds; 0 = no periodic report (`forge status` and each stage's end only).
        uint32 ProgressInterval = 0;

        /// The level every character is, or 0 for the curriculum's random levels (AnimusForge.Curriculum.Characters.*).
        /// Only the fast profile sets it (AnimusForge.Fast.Level).
        uint32 Level = 0;

        /// AnimusForge.Fast.*: the profile `forge fast` trains with (see FastProfile).
        uint32 FastEnvs = 16;
        /// AnimusForge.Fast.Budget: env steps each stage of a fast run trains for before the next one starts,
        /// overridden per invocation by `forge fast <steps>`. A fast run is a fixed-budget sweep of the whole
        /// curriculum, not a smoke test: it plays the same content at every class and every level, and only
        /// the budget is smaller. Fast.Level and Fast.Classes used to make the problem easier as well, which
        /// meant a fast pass rehearsed something the real build never trains.
        uint64 FastBudget = 20000000;
        /// AnimusForge.Fast.Queue: what `forge fast` trains when given no scenarios; empty = every curriculum stage.
        std::vector<std::string> FastQueue;
        std::string FastOutputDir;                  // resolved: never empty after Load
        std::string FastLearnerOverlay;             // resolved: never empty after Load
        std::vector<std::string> FastLearnerArgs;

        /// AnimusForge.Bench.*: what `forge bench` measures (see BenchProfile and Forge::CommandBench).
        struct BenchSettings
        {
            std::string Scenario = "stage8_duel";   // the scenario every trial runs
            std::string Policy = "fight";           // the local policy of the sim-only trials
            std::vector<uint32> Threads;            // MapUpdate.Threads values to try
            std::vector<uint32> Envs;               // AnimusForge.Envs values to try
            uint32 MaxEnvs = 256;                   // never try more envs than this
            /// A sim-only trial: long enough that every env ends several episodes inside the window, so the
            /// rebuild an episode end costs is counted at its real share, and short enough to sweep a grid.
            uint32 WarmupTicks = 128;               // decisions run before a trial is timed
            uint32 MeasureTicks = 384;              // decisions timed
            /// The learner trials of the second phase, which rank a handful of settings rather than a grid: the
            /// warm-up also covers the learner's start-up and first updates, and the longer window averages over
            /// several of them (one update per rollout_length decisions).
            uint32 LearnerWarmupTicks = 384;
            uint32 LearnerMeasureTicks = 768;
            uint32 MaxMemoryPercent = 80;           // skip bigger envs once the machine's memory is this used
            uint32 LearnerTop = 2;                  // sim trials re-timed with the learner (0 = none)
            std::vector<uint32> LearnerTorchThreads;    // torch thread counts to try with the learner
            std::string OutputDir;                  // resolved: <OutputDir>/bench, never empty after Load
        };

        BenchSettings Bench;

        [[nodiscard]] bool IsRemote() const { return Policy == "remote"; }

        /// What the scenario and its env pool take from these settings (animus-lib's StageSettings); `scenario`
        /// names the stage so its own env count (StageEnvs) can apply.
        [[nodiscard]] Animus::StageSettings Stage(std::string const& scenario = "") const;

        /// These settings for one `forge bench` trial: `envs` envs, everything in the bench output directory (so a
        /// trial never archives, seeds from or overwrites a real run), and a learner that only trains -- no
        /// evaluation, no seeding, no distillation -- when `remote`.
        [[nodiscard]] ForgeConfig BenchProfile(uint32 envs, bool remote, uint32 torchThreads) const;

        /// These settings with the fast profile applied: fewer envs and the learner's fast overlay, with every
        /// stage trained for `budget` env steps and no early convergence. Everything goes to FastOutputDir
        /// (runs, layouts and models), so a test run never archives, seeds from or overwrites a real run.
        [[nodiscard]] ForgeConfig FastProfile(uint64 budget) const;

        /// Where learners train: <OutputDir>/runs, runs/<scenario>/ per scenario.
        [[nodiscard]] std::filesystem::path RunsDir() const;

        /// Where scenarios write their layout manifests and stage descriptions: <OutputDir>/layouts.
        [[nodiscard]] std::filesystem::path LayoutsDir() const;

        /// Absolute learner config for a scenario: AnimusForge.Learner.Config if set, else configs/<scenario>.yaml.
        [[nodiscard]] std::string LearnerConfigFor(std::string const& scenario) const;

        void Load();
    };
}

#endif
