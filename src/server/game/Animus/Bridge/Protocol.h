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
 *                            (no terminator). ObsDim and NumActions are the largest layout's. SpecMsg ends with
 *                            u32 KinematicsDim, the floats per agent of each STEP's kinematics (protocol 20), and
 *                            u32 ImageBytes, the bytes per agent of each STEP's camera images, I below: the vision
 *                            block's height x width x 5 (protocol 23; x 4 at 21 and 22), 0 for a stage without one
 *                            (protocol 21); then u32
 *                            LookHeads, the look head's categoricals per agent in ACT (Vision::FreeLook::HEADS, 3)
 *                            with a vision block, 0 without one (protocol 22); then u32 MapBytes, the bytes per
 *                            agent of each STEP's mental map crops, M below: the map block's 48 x 48 x 6 (13,824), 0
 *                            for a stage without one (protocol 24).
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
 *                              f32 final_obs[D*A*O]   last obs of each ended episode: only the D envs
 *                                                     whose done is 1, in env order
 *                              f32 final_state[D*S]   last state of each ended episode, the same D envs
 *                              f32 episode_info[D*A*K] per agent totals of each ended episode, the same D
 *                                                     envs (protocol 18; every env's before)
 *                              u32 episode_seed[E]    evaluation seed index of the ended episode (valid if
 *                                                     done); NO_EPISODE_SEED for a training episode
 *                              f32 kinematics[E*A*M]  each agent's body after the transition, M = SPEC's
 *                                                     KinematicsDim (Kinematics::SAMPLE_DIM = 10): t (episode
 *                                                     seconds), x, y, z, yaw, pitch, mode (0 ground, 1 swimming,
 *                                                     2 flying, 3 airborne), mounted, speed in force (yd/s),
 *                                                     in_combat; the new episode's first sample where done is 1,
 *                                                     zeros for an agent without a body (protocol 20)
 *                            and, only in a stage with a vision block (I = SPEC's ImageBytes > 0; protocol 21):
 *                              u8  image[E*A*I]       each agent's camera image after any auto-reset, [row][col][byte]
 *                                                     row 0 at the top, 4 bytes a pixel (Vision::EncodePixel,
 *                                                     camera-vision.BYTES.md); for an agent with no frame,
 *                                                     Vision::FillNoFrame's pattern (sky, height 0). Absent
 *                                                     when the learner reads it from the device buffers (DEVICE)
 *                              u8  final_image[D*A*I] last image of each ended episode, the same D envs as final_obs
 *                            and, only in a stage with a map block (M = SPEC's MapBytes > 0; protocol 24):
 *                              u8  map[E*A*M]         each agent's mental map crop after any auto-reset: 48 x 48 cells
 *                                                     of 2 yd, heading-up, [row][col][channel], 6 bytes a cell
 *                                                     (Vision::CropChannel: code, height, visited, age, class,
 *                                                     frontier); all zeros for an agent with no map. Always on the
 *                                                     socket, even with DEVICE buffers (G3 has to carry it there)
 *                              u8  final_map[D*A*M]   last crop of each ended episode, the same D envs as final_obs
 *                            A stage without one sends exactly the protocol 20 STEP (SPEC grows by ImageBytes for
 *                            every stage).
 *   client -> server  ACT    { i32 actions[E*A] } or, from a policy with a goal head,
 *                            { i32 actions[E*A], i32 goals[E*A*2] } -- the goals each agent is pursuing, primary
 *                            then secondary (0..GoalCount-1, or -1 for none). Goals are scored and reported by the
 *                            scenario and shown to a party's teammates; they never mask an action.
 *                            With LookHeads L > 0 (a stage with a vision block; protocol 22) either is followed by
 *                              i32 look[E*A*L]        each agent's look head choice, agent-major in the actions'
 *                                                     order: yaw rate (0..6), pitch rate (0..4), zoom (0..4)
 *                                                     (Vision::FreeLook). Rows without a camera (a director, an
 *                                                     absent agent) send 0s, placeholders the sim never applies.
 *                                                     Required, and every value in range: anything else is a
 *                                                     protocol error and drops the learner.
 *                            A stage without one sends exactly protocol 21's ACT.
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
 * episodes and the learner's actions the rest (learner against a scripted opponent); with MODE_FLAG_STAND_IN, every
 * party has the "human" stand-in in one seat, a row the learner neither plays nor scores. MODE with Mode = 0 returns to
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
    // in, so a half-batch sim can send each half on its own.
    // 12: MODE says which seed index an evaluation starts from, so a cluster's sims share one evaluation's seeds.
    // 13: HELLO names the learner's rank and how many there are (data-parallel learners share one sim's pool).
    // 14: STEP's final_obs and final_state carry only the envs whose done is set: every other env's were ~half of
    // each STEP's bytes, for rows the learner never reads.
    // 15: DEVICE and DEVICE_ACK. After SPEC a sim with the device library offers the learner device buffers for obs,
    // state and mask by their IPC handles; a learner that opens them says so, and from then on its STEPs carry
    // neither (the sim writes them into the buffers first). A learner that declines keeps the socket path.
    // 16: a goal in ACT is a kind and a target, kind * GOAL_TARGETS + target (SPEC's goal count is the joint count),
    // where it was one of six kinds: an older learner would send kinds the sim reads as targets of the first kind.
    // 17: ACT carries two goals per agent, primary then secondary (GOAL_SLOTS_ON_WIRE), where it carried one; and
    // the goal space has a twelfth kind (Resurrect). The learner keeps the queue behind them itself.
    // 18: PROGRESS carries the shaping scale after the progress (ProgressMsg): a sim that took the old four bytes
    // would never fade its shaping, and nothing would say so. STEP's episode_info carries only the envs whose done is
    // set, as final_obs and final_state do: the others' were most of a STEP's bytes in a wide stage. MODE names a
    // held-out arena for the evaluation to play. EXPLORE_STARTS gives the wings the cells to start from (Go-Explore).
    // 19: PROGRESS carries the cost scale after the shaping scale (the learner's cost ladder on the noise prices).
    // 20: SPEC ends with the kinematics width and every STEP ends with one kinematic sample per agent (Kinematics.h):
    // the bodies the learner's style reward and realism score read. A learner of 19 would read the width as the
    // first episode info name's bytes and every STEP as too long.
    // 21: the camera's image travels as bytes (camera-vision.BYTES.md): SPEC ends with the image bytes per agent, and
    // a stage with a vision block (revision 3) ends each STEP with every agent's image and the ended envs' final
    // images, and its DEVICE message with the images' device buffer handle. The vision block's float columns are its
    // seven scalars. A stage without one has protocol 20's STEP and DEVICE; every SPEC is four bytes longer.
    // 22: free look (camera-vision.FREELOOK.md): SPEC ends with LookHeads, and a stage with a vision block (revision 4)
    // ends each ACT with every agent's look head choice. A stage without one has protocol 21's ACT; every SPEC is
    // four bytes longer.
    // 23: identity (perception-goals P2): a camera pixel is five bytes, the class and the entity slot (vision block
    // revision 5), so ImageBytes is height x width x 5; the entity list is a block of float columns. The messages'
    // layout is protocol 22's; a stage without a vision block is byte-identical to it but for the version.
    // 24: the mental map (perception-goals REDESIGN §3): SPEC ends with MapBytes, and a stage with a map block ends each
    // STEP with every agent's crop and the ended envs' final crops, after the images. A stage without one has protocol
    // 23's STEP; every SPEC is four bytes longer.
    constexpr uint32 PROTOCOL_VERSION = 24;
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
        Device = 9,
        DeviceAck = 10,
        /// client -> server ProgressMsg: how far through its budget the stage's training is, 0 to 1, and what shaping
        /// is paid times, sent after every update and applied without an answer. Arenas whose weights change over a
        /// stage (ArenaDefinition::WeightFinal) draw by the first; every Shaping reward term is paid times the second
        /// (RewardLedger::SetShaping), and every noise price times the third (RewardLedger::SetCosts, the cost
        /// ladder). Protocol 17; the shaping scale from 18, the cost scale from 19.
        Progress = 11,
        /// client -> server: ExploreStartsHeader, then Count ExploreCell -- the cells a training run of a dungeon wing
        /// starts from instead of the door (Go-Explore), Share of them, drawn by weight; replaces the last table,
        /// applied without an answer as envs reset. Evaluation never takes one. Protocol 18.
        ExploreStarts = 12,
    };

#pragma pack(push, 1)
    struct MsgHeader
    {
        uint32 Type;
        uint32 Length;
    };

    struct ProgressMsg
    {
        float Progress;
        float ShapingScale;
        float CostScale;
    };

    struct HelloMsg
    {
        uint32 Version;
        uint32 Rank;            // this learner's rank among Ranks data-parallel learners (0 of 1 alone)
        uint32 Ranks;
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
        uint32 KinematicsDim;   // floats per agent of each STEP's kinematics (Kinematics::SAMPLE_DIM; protocol 20)
        /// Bytes per agent of each STEP's camera images (ScenarioSpec::ImageBytes; 0 without a vision block;
        /// protocol 21): the wire's cut, which the learner checks against stage.json's image.
        uint32 ImageBytes;
        /// The look head's categoricals per agent in ACT (ScenarioSpec::LookHeads; 0 without a vision block;
        /// protocol 22). Their sizes are the vision block's manifest's "look" "heads".
        uint32 LookHeads;
        /// Bytes per agent of each STEP's mental map crops (ScenarioSpec::MapBytes; 0 without a map block; protocol
        /// 24): the wire's cut, which the learner checks against stage.json's map.
        uint32 MapBytes;
    };
    // The learner's SPEC (protocol.py): "<12I32s4I", 96 bytes.
    static_assert(sizeof(SpecMsg) == 12 * 4 + SCENARIO_NAME_SIZE + 4 * 4 && sizeof(SpecMsg) == 96);

    struct LayoutMsg
    {
        uint32 ObsDim;
        uint32 NumActions;
        char Name[LAYOUT_NAME_SIZE];
    };

    /// ModeMsg::Flags. SCRIPTED_OPPONENTS: the Baseline policy plays only the scenario's opponent seats (the other
    /// side of a self-play episode, see Scenario::IsOpponentSeat) and the learner's ACT actions play the rest.
    constexpr uint32 MODE_FLAG_SCRIPTED_OPPONENTS = 1;
    /// STAND_IN: every party of the evaluation has the "human" stand-in in one seat (dungeon-curriculum I7, the
    /// learner's "with the human stand-in" arm); without it an evaluation's parties are all bots. Training draws its
    /// own share (StandIn.Share) whatever this says.
    constexpr uint32 MODE_FLAG_STAND_IN = 2;

    struct ModeMsg
    {
        uint32 Mode;                        // 0 = training, 1 = evaluation
        uint32 SeedBase;
        uint32 Episodes;                    // seeded evaluation episodes
        uint32 Flags;                       // MODE_FLAG_*
        uint32 FirstSeed;                   // the evaluation plays seed indexes [FirstSeed, FirstSeed + Episodes)
        uint32 Arena;                       // a held-out arena to play, its index + 1; 0 = the stage's own (18)
        char Baseline[POLICY_NAME_SIZE];    // scripted policy to run instead of the learner's; empty = learner
    };

    /// WEIGHTS payload: Count, then that many float weights -- one per (class, spec), layout-major in the SPEC's
    /// order and spec-minor, so Count is the layout count times Curriculum::MAX_SPECS.
    struct WeightsHeader
    {
        uint32 Count;
    };

    /// EXPLORE_STARTS payload: this header, then Count cells (at most MAX_EXPLORE_STARTS).
    constexpr uint32 MAX_EXPLORE_STARTS = 64;

    struct ExploreStartsHeader
    {
        float Share;
        uint32 Count;
    };

    /// A cell of a wing run (InstanceEncounter's marks): the arena and its row (tier), the route's packs cleared as
    /// EXPLORE_PACK_WORDS words of 24 bits (route order, word-major), the party's yard on the route / 16, the weight.
    struct ExploreCell
    {
        uint32 Arena;
        uint32 Tier;
        uint32 Packs[4];
        uint32 Yard;
        float Weight;
    };
    // The learner's EXPLORE_STARTS and EXPLORE_CELL (protocol.py): "<fI" and "<II4IIf".
    static_assert(sizeof(ExploreStartsHeader) == 8 && sizeof(ExploreCell) == 32);

    /// REPLAY payload: this header, then Count uint32 evaluation seed indexes (at most MAX_REPLAY_SEEDS).
    constexpr uint32 MAX_REPLAY_SEEDS = 65536;

    struct ReplayHeader
    {
        uint32 SeedBase;
        float Fraction;
        uint32 Count;
    };

    /// Then the arrays of envs [EnvBegin, EnvBegin + EnvCount), in the order of the learner's Spec.step_layout.
    /// DEVICE: device buffers holding this rank's obs [E, A, O] float, state [E, S] float and mask [E, A, N] uint8,
    /// env-major in the rank's own env numbering, on HIP device `Device`. Handles are hipIpcMemHandle_t bytes. In a
    /// stage with a vision block the message is followed by one more handle, DEVICE_HANDLE_BYTES: the images
    /// [E, A, I] uint8 (protocol 21), which its STEPs then leave out as they leave out obs.
    constexpr uint32 DEVICE_HANDLE_BYTES = 64;
    struct DeviceMsg
    {
        uint32 Device;
        uint32 Envs;
        uint8 ObsHandle[DEVICE_HANDLE_BYTES];
        uint8 StateHandle[DEVICE_HANDLE_BYTES];
        uint8 MaskHandle[DEVICE_HANDLE_BYTES];
    };

    /// DEVICE_ACK: 1 when the learner opened the buffers and reads obs, state and mask from them from now on.
    struct DeviceAckMsg
    {
        uint32 Accepted;
    };

    struct StepHeader
    {
        uint64 Decision;
        uint32 EnvBegin;
        uint32 EnvCount;
    };

    /// ACT payload: this header, then EnvCount x AgentsPerEnv int32 actions, then GOAL_SLOTS_ON_WIRE as many goals
    /// when the policy has a goal head, then LookHeads as many look choices when the stage has a camera.
    struct ActHeader
    {
        uint32 EnvBegin;
        uint32 EnvCount;
    };
#pragma pack(pop)

    /// How an ACT's body (after its header) is cut, for `rows` agent rows: whether its length is one the stage
    /// takes, whether it carries goals, and where its look section starts. With lookHeads > 0 the look section is
    /// required; without, it is protocol 21's ACT exactly.
    struct ActCut
    {
        bool Valid = false;
        bool Goals = false;
        std::size_t LookOffset = 0;     // bytes into the body
    };

    [[nodiscard]] inline ActCut CutAct(std::size_t body, std::size_t rows, uint32 lookHeads)
    {
        std::size_t const actionBytes = rows * sizeof(int32);
        std::size_t const goalBytes = actionBytes * Animus::GOAL_SLOTS_ON_WIRE;
        std::size_t const lookBytes = actionBytes * lookHeads;
        ActCut cut;
        if (body == actionBytes + lookBytes)
            cut = { true, false, actionBytes };
        else if (body == actionBytes + goalBytes + lookBytes)
            cut = { true, true, actionBytes + goalBytes };
        return cut;
    }

    /// The first look row of `rows` (lookHeads values each) out of its head's range, or -1 when all are in range.
    [[nodiscard]] inline int64 BadLookRow(int32 const* look, std::size_t rows, uint32 lookHeads,
        uint32 const* headSizes)
    {
        for (std::size_t row = 0; row < rows; ++row)
            for (uint32 head = 0; head < lookHeads; ++head)
            {
                int32 const value = look[row * lookHeads + head];
                if (value < 0 || uint32(value) >= headSizes[head])
                    return int64(row);
            }
        return -1;
    }
}

#endif
