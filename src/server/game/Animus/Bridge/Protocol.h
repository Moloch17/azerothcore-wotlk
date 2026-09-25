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

/*
 * Lock-step wire protocol between the sim (server) and the Python learner (client).
 * Mirrored field for field in python/animus/protocol.py -- change both together and bump
 * PROTOCOL_VERSION.
 *
 * Every message is a MsgHeader followed by `length` payload bytes. Little-endian, no padding.
 *
 *   client -> server  HELLO  { u32 version }
 *   server -> client  SPEC   SpecMsg, then u32 layout count and that many LayoutMsg, then the episode info
 *                            column names as comma-separated ASCII filling the rest of the payload
 *                            (no terminator). ObsDim and NumActions are the largest layout's.
 *   server -> client  STEP   { u64 decision } then, in order, with E envs, A agents per env,
 *                            O obs dim, S state dim, N actions, K episode info dim:
 *                              f32 obs[E*A*O]         observation after any auto-reset
 *                              f32 state[E*S]         critic state after any auto-reset
 *                              u8  mask[E*A*N]        1 = action allowed
 *                              u16 layout[E*A]        each agent's layout (index into the SPEC's layouts):
 *                                                     it fills only that layout's first obs dim features
 *                                                     and action count mask entries; constant per episode
 *                              u8  present[E*A]       1 = the agent has a character this episode; 0 = an
 *                                                     empty seat (only the no-op, reward 0): not a sample
 *                              f32 reward[E*A]        reward for the transition that just ended
 *                              u8  done[E]            1 = episode ended on this transition
 *                              u8  terminated[E]      1 = it ended in a terminal state (no
 *                                                     bootstrap); done without terminated is a
 *                                                     time-limit truncation
 *                              f32 final_obs[E*A*O]   last obs of the ended episode (valid if done)
 *                              f32 final_state[E*S]   last state of the ended episode (valid if done)
 *                              f32 episode_info[E*A*K] per agent totals for the ended episode (valid if done)
 *                              u32 episode_seed[E]    evaluation seed index of the ended episode (valid if
 *                                                     done); NO_EPISODE_SEED for a training episode
 *   client -> server  ACT    { i32 actions[E*A] } or, from a policy with a goal head,
 *                            { i32 actions[E*A], i32 goals[E*A] } -- the goal each agent is pursuing
 *                            (0..GoalCount-1, or -1 for none). Goals are scored and reported by the scenario and
 *                            shown to a party's teammates; they never mask an action.
 *   client -> server  MODE   ModeMsg (instead of ACT) -- switch between training and evaluation; the server
 *                            resets every env and answers with a fresh STEP (zero reward and done)
 *   client -> server  WEIGHTS { u32 count, f32 weight[count] } (instead of ACT) -- how often training episodes
 *                            draw each (class, spec), layout-major in the SPEC's layout order and spec-minor in
 *                            the class's own spec order; the server applies them and waits for the ACT without
 *                            answering. A weight of 1 everywhere is the uniform draw; count must be the SPEC's
 *                            layout count times Curriculum::MAX_SPECS. A class with fewer specs than that still
 *                            has the slots, which are never drawn. Per pair and not per layout because one model
 *                            is a whole class: a paladin whose protection build wins and whose holy build does
 *                            not needs more holy episodes, not more paladin episodes -- and unlike the roles this
 *                            replaced, it also separates two builds that share a role, which is the case a
 *                            feral druid is.
 *   client -> server  REPLAY { u32 seed_base, f32 fraction, u32 count, u32 seed[count] } (instead of ACT) -- that
 *                            share of training resets rebuilds one of these evaluation seed indexes of seed_base, the
 *                            same character and opponent the evaluation built (the fight rolls afresh), in place of
 *                            the seeds sent before; count 0 or fraction 0 stops it. Applied without an answer, like
 *                            WEIGHTS. A replayed episode reports as a training episode (NO_EPISODE_SEED).
 *   client -> server  CLOSE  {}  (instead of ACT) -- server drops the client and waits for a new one
 *
 * Evaluation (MODE with Mode = 1): episodes seed index 0..Episodes-1 are handed out in order to the envs as
 * they reset, and the scenario builds each one right after reseeding the world thread's random numbers from
 * (SeedBase, index) -- the same characters and opponents every evaluation, whatever the env count. Envs that
 * reset once every index is handed out run unseeded episodes (NO_EPISODE_SEED). With a Baseline policy name
 * the sim ignores the ACT actions and runs that scripted policy instead, so the learner can score it on the
 * same seeds; with MODE_FLAG_SCRIPTED_OPPONENTS as well, the policy plays only the opponent seats of self-play
 * episodes and the learner's actions the rest (learner against a scripted opponent). MODE with Mode = 0 returns to
 * unseeded training episodes. Every new session (HELLO) starts in training mode, whatever mode the previous learner
 * left the sim in. Evaluation episodes always draw layouts evenly, whatever WEIGHTS asked for: seeded episode index
 * i plays (class, role) pair i % (pair count), so every one of them is scored on its own equal share of the
 * seeds -- one model per class, but a paladin's healing is measured apart from its tanking.
 *
 * The first STEP after SPEC carries freshly reset envs: its reward and done arrays are zero and
 * must not be recorded as a transition. A truncated episode (done, not terminated) bootstraps from
 * final_state; a terminated one does not.
 */

#ifndef MOD_ANIMUS_FORGE_PROTOCOL_H
#define MOD_ANIMUS_FORGE_PROTOCOL_H

#include "Define.h"
#include "EnvPool.h"
#include <bit>

namespace AnimusForge
{
    // 10: the WEIGHTS payload is per (class, spec) rather than per (class, role), which is a different length and
    // a different meaning for the same bytes -- a mismatched pair would silently misweight rather than fail.
    // 11: STEP and ACT name the envs they cover (StepHeader, ActHeader) and SPEC how many groups the pool is sent
    // in, so a half-batch sim can send each half on its own. This sim still sends the whole pool as one group.
    constexpr uint32 PROTOCOL_VERSION = 11;
    constexpr uint32 SCENARIO_NAME_SIZE = 32;
    constexpr uint32 POLICY_NAME_SIZE = 32;
    constexpr uint32 LAYOUT_NAME_SIZE = 48;
    using Animus::NO_EPISODE_SEED;      // the EpisodeSeed of a training episode (EnvPool.h)

    static_assert(std::endian::native == std::endian::little, "the wire protocol is little-endian");

    enum class MsgType : uint32
    {
        Hello = 1,
        Spec  = 2,
        Step  = 3,
        Act   = 4,
        Close = 5,
        Mode  = 6,
        Weights = 7,
        Replay = 8,
    };

#pragma pack(push, 1)
    struct MsgHeader
    {
        uint32 Type;
        uint32 Length;
    };

    struct HelloMsg
    {
        uint32 Version;
    };

    struct SpecMsg
    {
        uint32 Version;
        uint32 NumEnvs;
        uint32 AgentsPerEnv;
        uint32 ObsDim;
        uint32 StateDim;
        uint32 NumActions;
        uint32 EpisodeInfoDim;
        uint32 GoalCount;       // goals a policy may pursue and send with its actions; 0 = the scenario has none
        uint32 TickMs;
        uint32 DecisionTicks;
        uint32 EpisodeSeconds;
        uint32 EnvGroups;       // STEPs per decision: 1, or 2 for half-batch (contiguous halves, the first half first)
        char Scenario[SCENARIO_NAME_SIZE];
    };

    struct LayoutMsg
    {
        uint32 ObsDim;
        uint32 NumActions;
        char Name[LAYOUT_NAME_SIZE];
    };

    /// ModeMsg::Flags. SCRIPTED_OPPONENTS: the Baseline policy plays only the scenario's opponent seats (the other
    /// side of a self-play episode, see Scenario::IsOpponentSeat) and the learner's ACT actions play the rest.
    constexpr uint32 MODE_FLAG_SCRIPTED_OPPONENTS = 1;

    struct ModeMsg
    {
        uint32 Mode;                        // 0 = training, 1 = evaluation
        uint32 SeedBase;
        uint32 Episodes;                    // seeded evaluation episodes
        uint32 Flags;                       // MODE_FLAG_*
        char Baseline[POLICY_NAME_SIZE];    // scripted policy to run instead of the learner's; empty = learner
    };

    /// WEIGHTS payload: Count, then that many float weights -- one per (class, spec), layout-major in the SPEC's
    /// order and spec-minor, so Count is the layout count times Curriculum::MAX_SPECS.
    struct WeightsHeader
    {
        uint32 Count;
    };

    /// REPLAY payload: this header, then Count uint32 evaluation seed indexes (at most MAX_REPLAY_SEEDS).
    constexpr uint32 MAX_REPLAY_SEEDS = 65536;

    struct ReplayHeader
    {
        uint32 SeedBase;
        float Fraction;
        uint32 Count;
    };

    /// Then the arrays of envs [EnvBegin, EnvBegin + EnvCount), in the order of the learner's Spec.step_layout.
    struct StepHeader
    {
        uint64 Decision;
        uint32 EnvBegin;
        uint32 EnvCount;
    };

    /// ACT payload: this header, then EnvCount x AgentsPerEnv int32 actions, then as many goals when the policy has
    /// a goal head.
    struct ActHeader
    {
        uint32 EnvBegin;
        uint32 EnvCount;
    };
#pragma pack(pop)
}

#endif
