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

#ifndef ANIMUS_LIB_CURRICULUM_CURRICULUM_TUNING_H
#define ANIMUS_LIB_CURRICULUM_CURRICULUM_TUNING_H

#include "Define.h"
#include "ActionTuning.h"
#include "StandIn.h"
#include <boost/json/fwd.hpp>
#include <string>

/*
 * Every value that shapes what the curriculum trains on and what it is paid for: the characters, parties,
 * pulls it meets, and the reward weights. Each is the config key <prefix><key>, the prefix being
 * the host's (StageSettings::TuningPrefix: AnimusForge.Curriculum.<key> in mod_animus_forge.conf.dist, which documents
 * them, and Animus.Curriculum.<key> in mod-animus); the effective values are recorded in each stage's stage.json and so
 * in every run directory. Visit lists them once, for loading and for writing.
 *
 * Per-decision reward terms are tuned per 50 ms decision and scaled with StageSettings::DecisionMs.
 */
namespace Animus::Curriculum
{
    struct CurriculumTuning
    {
        /// The seat characters.
        struct CharacterTuning
        {
            uint32 HighLevelFirst = 61;         // levels at or above this are "high"
            int32 HighLevelChance = 50;         // percent of characters drawn from the high levels
            /// Levels at or below this are "low". With half the characters at 61-80 and the rest spread over every
            /// level, only one in eight was 1-20, and those fights were the ones lost most (stage1_duel at 20M: 69%
            /// won at 1-10, 77% at 11-20, 85%+ from 31 on).
            uint32 LowLevelLast = 20;
            int32 LowLevelChance = 15;          // percent of characters drawn from the low levels (the rest: any level)
            // How a character's talents are spent (see TalentBuilder). A standard build is always the same for a
            // spec and a level, so a policy trained on those alone has nothing to read in its talent features: some
            // characters move a few points, some spend them all at random, and the policy has to play what it got.
            int32 NoisyTalentChance = 30;       // percent of characters: the standard build with points moved
            int32 RandomTalentChance = 10;      // percent: every point spent at random (the rest: standard)
            uint32 TalentNoisePoints = 5;       // a noisy build moves 1 to this many of its last points
            // Percent of pet-class characters that start the episode with their pet out, as a player arrives with
            // one: the rest summon it themselves (or not).
            int32 PetOutChance = 50;
            /// Episodes a seat keeps its character for when the next episode draws the same class and build, before
            /// it is built afresh (race, level, talents, gear). Building a character was 6.4 ms of a 24 ms decision
            /// (reset 0.96 episodes per decision, stage1_duel at 128 envs); a kept character is healed, cleared of
            /// buffs and cooldowns, restocked and moved to the new spawn instead. Evaluations always build: their
            /// seeded spread of characters is the yardstick. 0 = build every episode.
            uint32 ReuseEpisodes = 4;
            /// 1: in training, a seat whose character can still be kept (ReuseEpisodes) keeps its class and build too,
            /// where a seat is drawn with no party makeup to honour, rather than drawing them and keeping the
            /// character only when the draw happens to repeat -- which with ten classes and their builds it did for
            /// 3.5% of stage2_indoor's episodes, whose resets were then nearly all character builds (0.4 ms each, the
            /// map update's tail). Each env's classes come in runs of up to ReuseEpisodes episodes; the mix across
            /// envs is the draw's. 0 = draw every episode.
            uint32 KeepCasting = 1;
        } Characters;

        /// Which party seats have a character, and their roles.
        struct PartyTuning
        {
            int32 ClassicChance = 50;           // percent: tank, healer, damage dealers instead of drawn roles
            int32 RoleTankChance = 25;          // drawn roles: percent tanks, healers, the rest damage dealers
            int32 RoleHealerChance = 25;
            // Rewards added to the owner's, per teammate.
            float TeammateDamageTakenDps = 0.5f;        // damage dealers: a non-tank teammate's damage taken
            float TeammateDamageTakenProtector = 1.0f;  // tanks and healers
            float TeammateHealing = 2.0f;               // healers: effective healing, fraction of its health
            /// A healer's pay for keeping the others up (TeammateHealing, Raid.KeepUp's above-half share) while it
            /// holds no Protect goal, as a share of the full. 1 pays it whatever the goal: at 0.25 healers held Protect
            /// 0.2% of the time and their healing was paid a quarter, which the overheal charge then outweighed
            /// (2026-10-03, stage6).
            float HealOffGoal = 1.0f;
            /// A party's or raid's tank: its damage dealt, as a share of what a damage dealer is paid for it. Holding
            /// the enemies (Raid.TankHold) is the tank's pay; hitting them is the damage dealers'.
            float TankDamageShare = 0.25f;
            float TankLoseTeammate = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
            /// Damage dealers and healers beside a living teammate that holds the pull: per enemy on the seat, per
            /// decision. Only the tank was charged when enemies reached the others, so in a party nobody else was
            /// ever told not to take aggro, and the teammate-threat charge sat at -33 a seat all through the second
            /// full run's party stage (2026-09-28). Owner.PulledThreat is the same charge beside a tank owner.
            float PulledThreat = 0.004f;
            float TeammateDeath = 3.0f;
        } Party;

        /// Raids (a raid's seat plan): the synthetic pulls sized to the raid, and each role paid for its own part
        /// -- a forty-seat raid paid only as a party learned to leave the fight to the others.
        struct RaidTuning
        {
            /// Tanks, party and raid: per enemy on the tank, per decision. 0.006 was drowned by the charges for loose
            /// enemies: Ragefire's tanks netted -9.6 a run on the threat term (2026-10-02).
            float TankHold = 0.015f;
            float TankLoose = 0.006f;           // ... charged per enemy on somebody else, per decision
            /// A party's damage dealers: their damage on the tank's target times this; and per enemy they have taken
            /// off the tank, per decision.
            float TankTarget = 0.3f;
            float PulledOff = 0.004f;
            /// A party's damage dealer or healer, per enemy on it while the party's tank is alive and not yet in
            /// combat, per decision (RewardTerm::EarlyPull, a cost): the pull opened before the tank engaged.
            float EarlyPull = 0.01f;
            /// Healers in a party or a raid: per member of its group above 50% health, per decision; the same
            /// charged per member below 35%. Per 50 ms of tuning (DecisionScale): four members kept up over a 300 s fight pay
            /// about 5, a kill's worth, not the 100-plus that 0.004 would have.
            float KeepUp = 0.0002f;
            /// Healers in a party or a raid: the healing they cast that landed on nobody's missing health, as a share
            /// of their own health, charged at this share of what effective healing pays (Party.TeammateHealing). At
            /// 0.5 a heal two-thirds wasted breaks even and anything less wasted pays. Charged at 2.0 outright (four
            /// times the off-goal pay, three times more in a drill) a heal half wasted cost five times what it earned,
            /// and the healers stopped healing (2026-10-03, stage6).
            float Overheal = 0.5f;
            /// Tanks in a party or a raid, per decision in a fight: in the spec's tanking stance, form or aura
            /// (Defensive Stance, Bear Form, Righteous Fury, Frost Presence). Warrior tanks finished 27 of 38 drill
            /// fights in Battle Stance (stage6 at 41M).
            float TankStance = 0.001f;
            /// Party and raid: per decision in a fight with an enemy in reach, once nothing the seat did served or
            /// was neutral -- no press, no damage, no healing -- for IdleMs. Per 50 ms of tuning: a minute idle
            /// costs about 1.2.
            float Idle = 0.001f;
            uint32 IdleMs = 4000;
            float IdleReach = 40.0f;            // yards: an enemy this near is one to act on
        } Raid;

        /// The range the approach shaping aims for.
        struct DuelTuning
        {
            float MeleeRange = 3.5f;            // the range the approach shaping aims for, melee specs
            float RangedRange = 25.0f;          // ... ranged specs
        } Duel;

        /// How hard the combat and role drills' opponents are, per class/role. A class/role moves up a tier when it
        /// wins RaiseAbove of Window fights at its tier, and down when it wins fewer than LowerBelow.
        struct DifficultyTuning
        {
            float RaiseAbove = 0.9f;
            float LowerBelow = 0.6f;
            uint32 Window = 200;                // fights at a tier before it is judged
            int32 ReviewChance = 25;            // percent of training fights drawn from a lower tier, so none is lost
            /// Percent of training fights drawn one tier *above* the class/role's own, which do not count towards
            /// moving it. An evaluation spreads its seeds over every tier, so a class/role that stalls is scored on
            /// fights it would otherwise never see: the rogue sat at tier 4 while 3 of its 7 evaluation tiers were
            /// elites it had never trained on, and lost 44% of them.
            int32 StretchChance = 10;
            /// Duel: the share of fights against something that casts, and of those, the share against something
            /// that puts a hazard on the ground. The duel pool is default-AI creatures, which never cast, so at 0
            /// stage 1 teaches nothing about interrupting, dispelling or stepping out of anything -- every one of
            /// those had to wait for stage 2's packs, where they compete with learning to fight several enemies.
            uint32 CasterChance = 40;
            /// The outcome terms scale with the tier: a win (kill, clear, health kept) is multiplied by
            /// 1 + TierScale x tier, a loss (death, timeout, overtime) divided by it. A tier-0 fight is unchanged;
            /// at tier 6 and 0.25 a kill pays 2.5x and a death costs 0.4x. Evaluations spread their seeds over
            /// every tier while training climbs per class, so with flat terms the score fell as the ladder rose
            /// -- every rung-6 loss cost as much as a rung-0 one -- and convergence read the fall as done. Scaled,
            /// the break-even win rate falls with the tier, so a hard fight is worth attempting, and the score is
            /// comparable across rungs. Fixed-bonus opponents (evade, hide, stealth) are not a ladder and stay flat.
            float TierScale = 0.25f;
        } Difficulty;

        /// Whole dungeon wings (InstanceEncounter): what a run is paid and charged, and how its rungs step.
        struct InstanceTuning
        {
            /// The rung's tier scale is capped here: a ladder of twenty bosses at 0.25 a tier would pay a top kill
            /// 5.75x, where the pool ladders stop at 2.5x. Kill, HealthKept and BossProgress are multiplied by the
            /// capped scale, Death and Timeout divided by it.
            uint32 MaxTierScale = 6;
            /// Whole wings (InstanceLadder::Wing): each trash creature killed, the wing's boss, each seat's death
            /// and each wipe (WingWipes of them end the episode; below that a wipe stands the party up at the door).
            /// One since 2026-09-30: a wipe ends the run, and the next run starts in a fresh instance with the whole
            /// dungeon reset -- never a second attempt on a half-cleared one. The kill terms scale with the rung, the
            /// costs are divided by it. No term is paid for ground covered: there is no route, waypoint or path
            /// progress (vision-only movement, 2026-10-08).
            /// Raised 2026-09-30 so a wing pays for itself: over a 1,200 s run the per-press costs (jitter, repeat,
            /// effort, the combat clock) came to about -75, against 7.5 for 25 trash kills and 10 for a last boss
            /// no party reached. A dungeon takes long to learn; every step of it has to be worth taking.
            float WingTrashKill = 1.0f;
            float WingBoss = 25.0f;
            float WingMidBoss = 8.0f;           // each dungeon boss killed on the way to the last
            float WingDeath = 3.0f;
            float WingWipe = 5.0f;
            /// The wipe that ends the run (2 since 2026-10-07, the dungeon curriculum's rule: no stage ends at the
            /// first
            /// death, and the Deadmines' bar is at most one wipe -- the first is scored and the party rises at the
            /// entrance; the second ends it). The ladder's rungs spare more (WingRung::ExtraWipes).
            uint32 WingWipes = 2;
            /// Per second once WingStallGraceMs pass with no kill and nothing fighting the party. Standing at the door
            /// has to cost more than fighting through the dungeon badly. Paid as Idle (a Cost, at its full price
            /// from the first step) since 2026-10-07: as Stall it was Shaping, and the fade took it away.
            float WingStall = 0.1f;
            uint32 WingStallGraceMs = 60000;
            /// The stall charge on every seat but the tank, as a share of the tank's: the tank decides when to move on.
            float WingStallOthers = 0.2f;
            /// Paid to every seat for each fight started with every living seat at WingReadyShare of its health and
            /// mana -- the rest discipline of a party (pull, fight, rest, ready, next) -- at most once a run, times
            /// the tier scale: ReadyPull, an Outcome since 2026-10-07 (it was the tank's Threat, Shaping).
            float WingEngage = 1.0f;
            float WingReadyShare = 0.8f;
            /// The clock out costs this times the share of the dungeon left uncleared (the creatures not killed, over
            /// the tier scale): a kill/clear share, no route.
            float WingTimeout = 30.0f;
            /// The dungeon curriculum's party stages (2026-10-07): a run that killed every creature its full clear
            /// counts and the last boss (Clear, times the tier scale: D2's and D3's full clear); every second of a run
            /// (StepCost: the clock, at its full price).
            float WingClear = 25.0f;
            float WingClock = 0.002f;
            /// The difficulty ladder (StageScenario::WING_RUNGS): each rung fixes the level lift and the wipes to
            /// spare. WingProbe of training runs are probes, and only they measure the policy: once WingRungRuns
            /// probes at a rung have made, on average, WingRungTarget
            /// of the dungeon (the share of its creatures killed, 1 for a clear), the ladder steps down. It never steps
            /// back on a score (a harder rung scores lower by design); WingLadder warns when the probes stay under a
            /// floor for three reads. Nothing on a clock. In a cluster the host's ladder decides for every machine,
            /// from all their runs.
            float WingProbe = 0.2f;
            uint32 WingRungRuns = 40;
            float WingRungTarget = 0.6f;
            uint32 WingRungStart = 0;           // the rung a run starts on (a resumed run names the one it reached)
            uint32 WingSupplies = 60;           // food and drink each seat brings into a whole dungeon
            /// Log a line for each wipe: where, what was fighting the party, and who died in what order.
            uint32 WingTrace = 1;
            /// Per second, for each hostile creature on the party past WingCrowdFree (a pack): the pull that ran into
            /// the next one. The Deadmines' parties had a median of eight on them when they wiped (2026-10-01).
            float WingCrowd = 0.15f;
            uint32 WingCrowdFree = 4;
            /// Per second a seat other than the tank is further than WingStrayYards from it (both alive): stay with the
            /// leader. Paid as Lost (a Cost) since 2026-10-07: as Approach it was Shaping, and the fade took it away.
            float WingStray = 0.02f;
            /// Per second a seat is dead or walking back from the entrance after a rise (Away, a Cost, as C3's
            /// Combat.Away): the time a death costs the party. Lost is never charged on the same seconds.
            float WingAway = 0.02f;
            float WingStrayYards = 25.0f;
        } Instance;

        /// The learner's goals (SeatGoal), in every stage that its policy chooses them for.
        struct GoalTuning
        {
            /// Paid once when the goal the seat holds is reached (GoalBlock::Status: the enemy it named is dead, the
            /// friend is healthy, the place is reached, the objective is done), not for holding it: a goal is there
            /// to be reached, and paying to sit in one made standing at range the stage's second largest earner.
            /// It replaced Goals.Match, which paid on the first decision a goal was merely held. Small on purpose:
            /// it keeps the goals apart and makes reaching one the point, and does not pay for play the stage's own
            /// terms price.
            float Reached = 0.05f;
            /// Charged each time the goal head changes a seat's goal kind. Reaching pays once per goal, so a head that
            /// switched goals at every choice collected it again each time: churn was paid, and in the first full
            /// run's combat stages the head kept its goal only 22% of the time against chance's 17% (2026-09-28). A
            /// switch costs a little more than a match earns, so changing goal has to be worth it on the stage's own
            /// terms -- which is what committing to a plan means. Charged for any change of a goal still in progress,
            /// its kind or its target (a goal that ended is replaced free), and raised from 0.03 when the next-run
            /// trial's goals stayed near random (2026-09-30).
            float Switch = 0.15f;
            /// Progress toward the goal held (potential-based, so it cannot be farmed): Progress x (gamma x phi' -
            /// phi) every decision, phi in [-1, 0] per kind -- the yards left to its place (over 60), the named
            /// enemy's health, the seat's own health and mana for Recover and Rest, the friend's health for Protect.
            /// A goal chosen and walked away from costs what closing on it pays. ProgressGamma is the learner's gamma.
            float Progress = 0.5f;
            float ProgressGamma = 0.999f;
            /// What reaching a goal is worth, by what it achieved (paid once, in place of Reached, which stays for
            /// the kinds that have no value of their own): a named enemy dead, one held in crowd control, a friend
            /// brought back up, a place reached. Recover and Rest pay by the share
            /// of health and mana they restored since the goal was chosen. A flat 0.05 against episode returns of
            /// 10-50 made choosing well nearly worthless: in groups 3-8% of chosen goals were reached.
            float FightValue = 0.3f;
            float ControlValue = 0.2f;
            float RecoverValue = 1.0f;
            float ProtectValue = 1.0f;          // raised from 0.2: healers chose Protect 1% of the time
            /// Protect is reached by keeping its friend above half health while it is attacked for this long, as well
            /// as by healing it back above 70%.
            uint32 ProtectHoldMs = 5000;
            float TravelValue = 0.1f;
            /// The secondary goal (a second slot beside the primary: Fight A and hold B): paid this share of what the
            /// primary would be for reaching it and for closing on it, and charged Secondary every decision it is held,
            /// so covering everything is not free.
            float SecondaryShare = 0.5f;
            float Secondary = 0.002f;
        } Goals;

        /// Looking after itself and its friends, in every stage.
        struct SupportTuning
        {
            /// Per decision: the bot's effective healing on itself, and the damage its own absorbs soaked and its own
            /// damage-taken reductions prevented on itself, as fractions of its health. Kept below every stage's
            /// DamageTaken, so a heal recovers part of what the hit cost and taking damage to heal it back never pays.
            /// (Healing and protecting the owner and teammates pay through Owner.Healing and Party.TeammateHealing.)
            float SelfHealing = 0.5f;
            /// Per fraction of the mana pool spent on healing, charged wherever the healing reward is paid. Healing
            /// is worth what it restores for what it costs, and the cost was never priced: an effective heal of 3%
            /// of a health bar cost the same as one of 20%, so a rank choice (CoreBlock::ACTION_RANK_TIERS) bought
            /// nothing and heals over time could be stacked on a full bar for free. With it, the objective is
            /// healing per mana -- a heal landing 20% for 15% of the pool pays 0.085, the same heal landing 3%
            /// pays nothing.
            ///
            /// Kept well below SelfHealing on purpose: the failure to avoid is a seat that heals too little and
            /// dies, which costs 10. Watch deaths before efficiency when this moves.
            float HealingMana = 0.1f;
        } Support;

        using ActionTuning = ::Animus::Curriculum::ActionTuning;
        using OptionTuning = ::Animus::Curriculum::OptionTuning;

        /// How often a seat may press the same button, as a player would (see ActionTuning).
        ActionTuning Actions;

        /// How long each durative action may run before the seat has to choose again (see OptionTuning).
        OptionTuning Options;

        /// Ground effects: damage from something standing on the ground rather than aimed at the seat (a fire pool,
        /// a poison cloud, a consecration). Charged on top of DamageTaken, which already charges it once as damage,
        /// because this is the damage a seat could have walked out of -- it is the only term that pays for moving,
        /// and it is what makes "step out of it" learnable at all. It reads zero wherever nothing puts anything on
        /// the ground, which is most of the curriculum today and none of a dungeon.
        struct HazardTuning
        {
            float Damage = 0.5f;                // per fraction of the seat's maximum health taken from a hazard
            /// Per second standing in one, whether or not it has ticked yet. The damage alone is small, late and
            /// noisy -- it arrives after the decision that put the seat there -- while the seconds are immediate and
            /// describe the behaviour, as Spacing does for a ranged spec caught in melee.
            float Standing = 0.15f;
            /// At most this much an episode, both terms together. Melee have to stand in melee: a hazard under the
            /// enemy is a real trade, and an uncapped charge would teach a seat to leave the fight instead, which is
            /// worse than standing in fire.
            float Max = 3.0f;
        } Hazards;

        /// The movement stages' marker prices and stops (SightEncounter, SeekEncounter, InteractEncounter): the arrival
        /// and the time it takes, the distance and facing shaping, and what counts as stopped. (The marker courses'
        /// ladders went with the first movement curriculum.)
        struct MarkerTuning
        {
            float Arrive = 3.0f;                // per marker stopped on (Outcome)
            /// Per decision (Cost), per 50 ms of tuning (DecisionScale): the time a run takes. M1 is "as fast
            /// as possible" (the user, 2026-10-05), so time is priced to matter beside Arrive 3: about 0.04 a
            /// second, a straight 16 s hallway run nets about +2.4, a 40 s wander +1.4, and the 60 s clock
            /// running out costs 2.4. Paid in full, never on the noise ladder. It was 0.0002 (0.004 a second),
            /// which the discount alone outweighed.
            float StepCost = 0.002f;
            float Death = 3.0f;
            /// Potential shaping on the straight-line distance to the marker, spread over the leg so closing the
            /// whole of it pays Progress once (Shaping: M1's ground is open, the straight line is the way).
            float Progress = 1.0f;
            /// Potential shaping on the cosine of the marker's bearing from the seat's facing: Facing for turning
            /// from dead away to dead ahead (Shaping).
            float Facing = 0.25f;
            /// Stopped: no forward, back, strafe or vertical key held, no jump pending, on the ground, and the feet
            /// moved less than this many yards since the last decision.
            float StopMoved = 0.05f;
            /// How near a stop has to be to count in stop_distance (a stop far from the marker is a pause, not a try).
            float StopNear = 10.0f;
            /// Arriving is on the marker's own floor too: the unit within this many yards of its height (a seat under
            /// a ledge, or a storey below, is not on it).
            float ArriveRise = 2.0f;
        } Markers;

        /// **Death in an instance** (dungeon-curriculum I4; EntranceRespawn): a seat that dies is out for DelayMs, then
        /// stands up alive, at full health and power, at the instance's entrance (the map's entrance trigger), and
        /// plays on: no graveyard, ghost or corpse run, no teleport to the party, no end to the episode. It walks
        /// back on the controller. It has rejoined once it is within RejoinYards of the party's leader (or of the
        /// living party's centroid when the leader is down).
        struct RespawnTuning
        {
            uint32 DelayMs = 10000;
            float RejoinYards = 15.0f;
        } Respawn;

        /// **The party follow** (Opposition::PartyFollow, M4 move4_follow; dungeon-curriculum I5): a party of
        /// followers keeps with a leader walking a dungeon's route from the door to its last boss, stopping at each
        /// boss's place for StopSeconds of the rung. The leader is in the owner's slot, moved by the player controller:
        /// the script's keys (the route planner's corners toward the next stop -- a script, never a bot input) or, a
        /// CastShare percent of training episodes, a frozen checkpoint's (the learner's cast.agents.leader).
        ///
        /// **The ladder** (the shaping fade's rungs, SightDraw::Rung): the leader walks below WalkRungs; from
        /// SuddenFromRung it also stops where nobody expects it, for SuddenStopMinMs-SuddenStopMaxMs, every
        /// SuddenGapMin-SuddenGapMax seconds; from BackStepFromRung some of those stops step back BackStepYards first.
        ///
        /// Paid to each follower: Kept per second within [BandMin, BandMax] yards of the leader (FollowKept,
        /// Outcome); Regroup at each stop of the leader's of at least RegroupMinStopMs, once, on coming into the band,
        /// times 1 - seconds/RegroupWindow (Outcome: the leader's stops are the leader's, so it cannot be farmed);
        /// Lost per second past LostYards and Blocking per second within BlockYards ahead of a moving leader, inside
        /// BlockHalfAngle degrees of its facing (Costs); Death per death (Cost); Stuck and Wall at their own fixed
        /// price (Seek.*: off the cost ladder). The minimap shows party members within MinimapYards.
        struct PartyFollowTuning
        {
            float BandMin = 3.0f;
            float BandMax = 10.0f;
            float LostYards = 40.0f;
            float Kept = 0.02f;
            float Lost = 0.02f;
            float Regroup = 0.5f;
            float RegroupWindow = 20.0f;
            uint32 RegroupMinStopMs = 2000;
            float Blocking = 0.05f;
            float BlockYards = 2.5f;
            float BlockHalfAngle = 45.0f;
            float Death = 3.0f;
            float MinimapYards = 60.0f;
            uint32 WalkRungs = 1;
            uint32 SuddenFromRung = 2;
            uint32 BackStepFromRung = 3;
            float StopSecondsFirst = 8.0f;
            float StopSecondsLast = 3.0f;
            uint32 SuddenStopMinMs = 1000;
            uint32 SuddenStopMaxMs = 4000;
            float SuddenGapMin = 12.0f;
            float SuddenGapMax = 30.0f;
            float BackStepYards = 4.0f;
            int32 BackStepChance = 30;
            uint32 GiveUpMs = 6000;
            int32 CastShare = 0;
        } PartyFollow;

        /// The seek stage (Opposition::Seek, M2: SeekEncounter): one real object in one of the Stockades' rooms, found
        /// by sight. Arrive (Outcome) is paid once, on stopping within the arena's SeekRadius of the object (on its
        /// floor: within ArriveRise yards of its height). StepCost (Cost, per 50 ms of tuning) prices the time: a
        /// whole 300 s episode costs StepCost x 6000 = 3, as much as finding the object pays, and Death (6) is dearer
        /// than any clock, so dying is never a way out of the search. Stuck, Wall and WallSlide are the ground course's
        /// noise prices, on from the first step at their own fixed price, off the cost ladder (REDESIGN §2: "wall and
        /// stuck on from the start"; amendment 8: small next to Arrive, as M1's).
        ///
        /// The training-only aids (Shaping, faded away with the rest): Sighting once, on the first frame the camera
        /// shows the object's flag; NewGround for each NewGroundCell-yard cell of floor first walked onto before it is
        /// found; RoomSeen once for each room whose floor the camera's frame first shows this episode (at least
        /// RoomSeenRays of its cast rays hitting the room's floor: "looked into a room", REDESIGN §2), by the episode's
        /// own bookkeeping and never the remembered map's (amendment 6), so a map kept from before takes nothing away.
        /// Nothing is shaped on the object's distance: it is hidden, and a distance potential would be a compass in the
        /// reward that vanished at the fade.
        ///
        /// **The ladder** (SeekDraw::Rung): the hallway, a front cell's doorway, a front cell, deep; each rung keeps
        /// CarryShare of the one below. RungSeconds0-3 are the episodes' lengths by rung (the placement's). A hallway
        /// object stands on a hallway point HallwayNearest to HallwayFurthest yards from the seat, in sight of its eye
        /// (SightDraw::Place); a doorway object DoorwayInside yards in from the opening toward the room's centre, up to
        /// DoorwayDeeper more, up to DoorwaySpread either side.
        ///
        /// Placement: up to Attempts points drawn uniformly over the room's floor polygon, each kept when the floor
        /// below it (vmaps) is within FloorTolerance yards of the room's and nothing solid stands within Clearance
        /// yards of it, at knee height, along the four axes (the room's centre otherwise).
        ///
        /// **Room goals** (M2 goals plan, 2026-10-09; Goals 1, the temporary experiment switch, 0 for none): the goal
        /// head is offered the rooms the seat's own frames showed (a first frame with GlimpseRays floor rays on a
        /// room's polygon) as place targets, six slots bound in the order of the glimpses, and the nearest frontier of
        /// its mental map as the way on, at placement rungs from GoalsFromRung up. A room is checked when the seat
        /// stood in it for EnterDwellMs or CheckedShare of its floor cells (3 yd) were hit by this episode's rays.
        /// RoomGoal and RoomSwitch are Aid (RewardCategory::Aid): a held room goal whose room becomes checked after the
        /// choice pays RoomGoal, a room goal given up for another costs RoomSwitch, both times the aid scale
        /// max(0, 1 - progress / AidUntil). Return (Cost, fixed price) prices going back into a visited room after at
        /// least ReturnAwayMs outside its polygon by ReturnAwayYards.
        ///
        /// **Cell goals** (free choice goals, 2026-10-09; GoalSource 1, the default, the temporary A/B switch against
        /// the room slots of GoalSource 0, deleted with the loser): the goal head names a block (4 yd) of the seat's
        /// own mental-map crop instead of a room, and a plan is up to four of them in order. The sim latches the
        /// block's point at the choice. A cell goal is reached within CellReach yards of its point (2D) and CellRise
        /// yards of its height; a choice within CellSame yards of the goal held and not ended is that goal chosen
        /// again, free; and a goal whose best distance gains less than CellPatienceYards in CellPatienceMs is lost.
        /// CellGoal (once per goal reached, only for one chosen at least CellMinYards away, on a block the seat had
        /// not stood on) and CellProgress (per yard of new best closeness to the point, the same blocks) are Aid,
        /// times the aid scale; CellSwitch (a cell goal given up for another), CellLost (a cell goal lost) and
        /// CellStale (a block chosen that the seat had stood on) are Cost at fixed prices, in the score.
        ///
        /// **Explore, don't circle, get unstuck** (2026-10-10, decision 0023). Explore (category Exploring) pays
        /// ExploreSeen for each 2-yd cell of floor a ray of the seat's camera lands on for the first time this episode
        /// (the first look, from the spawn, is not paid), times ExploreRoomBonus in a room of the table not yet
        /// entered, until ExploreCap (the nominal sum, before the scale) is reached. FrontierPull (Exploring) pays
        /// FrontierPull per yard closed on the nearest frontier of the seat's own mental map, by a best-distance
        /// ratchet kept per frontier cluster (a ping-pong between two farms nothing), until FrontierCap. Both are paid
        /// times max(the shaping scale, ExploreFloor), so the fade leaves ExploreFloor of them. Circling (Cost, fixed
        /// price, per second, in the score) is charged while the last CircleWindowMs of decisions hold CircleYards of
        /// path or CircleTurnDeg of turning with less than CircleNetYards of net displacement; not on a decision that
        /// charged Stuck. The trap drill: in a training episode (never an evaluation's) with probability TrapShare the seat
        /// starts 0.5-1.5 yd from the jamb of a random door of the room table, facing it; Escape (Aid) is paid once
        /// when it is TrapEscapeYards from there within TrapEscapeMs.
        struct SeekTuning
        {
            float Arrive = 3.0f;
            float StepCost = 0.0005f;
            float Death = 6.0f;
            float ArriveRise = 2.0f;
            float Sighting = 0.5f;
            float NewGround = 0.004f;
            float NewGroundCell = 4.0f;
            float Stuck = 0.02f;                // per second, fixed price
            float Wall = 0.02f;                 // per second at no movement, fixed price, scaled as WallSlide says
            float WallSlide = 0.5f;
            uint32 Attempts = 24;
            float FloorTolerance = 2.0f;
            float Clearance = 0.8f;
            float RoomSeen = 0.1f;
            uint32 RoomSeenRays = 3;
            float CarryShare = 0.1f;
            uint32 RungSeconds0 = 90;
            uint32 RungSeconds1 = 120;
            uint32 RungSeconds2 = 200;
            uint32 RungSeconds3 = 300;
            float HallwayNearest = 8.0f;
            float HallwayFurthest = 120.0f;
            float DoorwayInside = 2.0f;
            float DoorwayDeeper = 1.5f;
            float DoorwaySpread = 1.0f;
            uint32 Goals = 1;
            uint32 GoalsFromRung = 2;
            float RoomGoal = 0.05f;
            float RoomSwitch = 0.03f;
            float Return = 0.05f;
            float AidUntil = 0.4f;
            uint32 GlimpseRays = 1;
            float CheckedShare = 0.6f;
            uint32 EnterDwellMs = 1000;
            float ReturnAwayYards = 8.0f;
            uint32 ReturnAwayMs = 2000;
            uint32 GoalSource = 1;
            float CellReach = 4.0f;
            float CellRise = 3.0f;
            float CellSame = 6.0f;
            float CellMinYards = 8.0f;
            uint32 CellPatienceMs = 20000;
            float CellPatienceYards = 2.0f;
            float CellGoal = 0.05f;
            float CellProgress = 0.004f;
            float CellSwitch = 0.02f;
            float CellLost = 0.03f;
            float CellStale = 0.01f;
            float ExploreSeen = 0.002f;
            float ExploreCap = 1.0f;
            float ExploreFloor = 0.5f;
            float ExploreRoomBonus = 2.0f;
            float FrontierPull = 0.004f;
            float FrontierCap = 1.0f;
            uint32 CircleWindowMs = 6000;
            float CircleYards = 12.0f;
            float CircleNetYards = 4.0f;
            float CircleTurnDeg = 540.0f;
            float Circling = 0.02f;
            float TrapShare = 0.12f;
            float Escape = 0.3f;
            float TrapEscapeYards = 6.0f;
            uint32 TrapEscapeMs = 20000;
        } Seek;

        /// **M3 interact** (Opposition::Interact, InteractEncounter; dungeon-curriculum M3): in an empty Deadmines, the
        /// object the goal names (by its kind, never its place) among DecoysMin to DecoysMax decoys of other kinds,
        /// behind a door whose lever opens it, or the lock its key item opens (the cannon and the gunpowder the seat
        /// carries from the start). Arrive (Outcome) is paid once, on the right object reached -- stopped within the
        /// arena's SeekRadius of it, on its floor (ArriveRise) -- or, for a lock, its key item used on it; DoorOpened
        /// (Outcome) once, the switch rung's door opened by the seat's own press on its lever. WrongObject (Cost) is
        /// charged once a decoy, stopped beside or pressed; a press the world refuses is the sight block's own price
        /// (Actions.Aimless.ActRefused): a locked door pressed, a press out of reach, the key on the wrong thing.
        /// StepCost (per 50 ms), Death, and Stuck, Wall and WallSlide (the ground course's, at their own fixed price
        /// from the first step) as the seek stage's. Sighting (Shaping, faded) once, on the first frame that lists
        /// the named object.
        ///
        /// **The ladder** (InteractDraw::Rung, the shaping fade's rungs 1, 0.5, 0): distinguish, switch, key; each rung
        /// keeps CarryShare of the one below, its episodes RungSeconds0 to 2 long. The distinguish rung's objects stand
        /// SightNearest to SightFurthest yards from the seat in sight of its eye, Spacing yards apart at least; up to
        /// Attempts draws. The cannon's script summons two pirates when it fires: every summoned creature within
        /// SummonSweep yards of a site's opener is sent away at each reset.
        struct InteractTuning
        {
            float Arrive = 3.0f;
            float DoorOpened = 1.0f;
            float WrongObject = 0.5f;
            float StepCost = 0.0005f;
            float Death = 6.0f;
            float ArriveRise = 2.0f;
            float Sighting = 0.5f;
            float Stuck = 0.02f;                // per second, fixed price
            float Wall = 0.02f;                 // per second at no movement, fixed price, scaled as WallSlide says
            float WallSlide = 0.5f;
            float CarryShare = 0.1f;
            uint32 RungSeconds0 = 60;
            uint32 RungSeconds1 = 120;
            uint32 RungSeconds2 = 90;
            uint32 DecoysMin = 2;
            uint32 DecoysMax = 4;
            float Spacing = 2.5f;
            float SightNearest = 4.0f;
            float SightFurthest = 30.0f;
            uint32 Attempts = 64;
            float SummonSweep = 80.0f;
        } Interact;

        /// **M1 controls, redesigned** (Opposition::Sight, SightEncounter; perception-goals REDESIGN §1): a real object
        /// along the Stockades' hallways, Nearest to Furthest yards (straight) from a random hallway spawn, in sight of
        /// the seat's eye -- or, from CornerFrom on the fade's ladder (1 - the shaping scale), with CornerShare of the
        /// episodes, just round a corner: out of sight of the spawn, in sight of a hallway point within CornerStep
        /// yards of it. Arrive, StepCost, Death, Progress, Facing, StopMoved, StopNear and ArriveRise are Markers.*.
        ///
        /// Arriving is stopping with the feet within the object's bounding radius plus ArriveTolerance of its centre
        /// (straight, on its floor). The body cannot stand inside the object (the controller meets its collision
        /// model; the body's own radius is 0.389 yd), so 1 yd of tolerance leaves at most about 0.6 yd of air between
        /// the body and the object's widest side: touching distance, M1's "within a yard of the mark" for a thing.
        ///
        /// The compass is withheld for the episode (its presence column and every value 0) with Withhold<i> at the
        /// fade's rung i (scales 1, 0.5, 0.25, 0; SightDraw::WithholdChance): an absent input, never a mask. The
        /// defaults 0.25, 0.6, 0.9, 0.9 start the ladder with a quarter of the episodes already without it, every one
        /// of them paid the dense shaping (the rung's scale), and fade the shaping alone on the last step: the seat
        /// that has to go by the camera is never left without the reward that teaches it to look
        /// (stage1-vision analysis, Design A).
        ///
        /// Wall and Stuck are charged at their own fixed price from the first step -- off the cost ladder
        /// (RewardLedger::AddFixed), unlike the ground course's -- and small beside Arrive 3: pinned to a wall for a
        /// whole second costs Wall + Stuck = 0.04, the step cost's own 0.04 a second again; ten seconds of it 0.4, an
        /// eighth of an arrival. To be set from M1's first evaluation (REDESIGN amendment 8).
        struct ControlsTuning
        {
            float Nearest = 10.0f;
            float Furthest = 120.0f;
            float ArriveTolerance = 1.0f;
            float CornerShare = 0.25f;
            float CornerFrom = 0.75f;           // 1 - the shaping scale: rung 2 (x0.25) on
            float CornerStep = 8.0f;
            uint32 Attempts = 64;
            float Withhold0 = 0.25f;
            float Withhold1 = 0.6f;
            float Withhold2 = 0.9f;
            float Withhold3 = 0.9f;
            float Stuck = 0.02f;                // per second, fixed price
            float Wall = 0.02f;                 // per second at no movement, fixed price, scaled as WallSlide says
            float WallSlide = 0.5f;
        } Controls;

        /// **The combat stages** (Opposition::Combat, CombatEncounter; dungeon-curriculum C1-C3): creatures on a
        /// cleared Ragefire Chasm, fought by a seat that sees them (I3).
        ///
        /// **Outcome**, each multiplied by the difficulty tier's w = 1 + Difficulty.TierScale x tier: Kill for each
        /// creature of C1 killed, Clear for each pack of C2 and C3 cleared, Survived at the episode's end with no death
        /// in it (Survived, or SurviveSurvived in C3, where staying alive is the lesson); InterruptLanded for an
        /// interrupt that stopped a cast (C2 and C3), unscaled. **Cost**, at a fixed price from the first step: Away per
        /// second dead or away from the fight (walking back from the entrance, or beyond AwayYards of the pull while it
        /// fights; never a reward for walking back, which would pay dying), Death (divided by w), AllyDeath (the guard arena's friend, divided by w), Hurt per maximum health taken (small),
        /// FireHurt per maximum health taken from ground effects, ExtraPull for a second pack drawn into a fight before
        /// the first is cleared, and Clock per second a creature of the current pull is alive and engaged (the time a
        /// kill takes). **Shaping** (faded): Damage per share of a creature's health the seat (or its pet) took off it.
        ///
        /// **The ladder** (DifficultyLadder, per class and build, steps on its own window's win rate alone): rung t of
        /// MaxTier puts the creatures at the seat's level + LevelBase + t x LevelsPerTier (C2 at half the steps, its
        /// pack growing instead), elites from EliteTier, a caster in every pack from CasterTier and linked packs from
        /// LinkedTier; HazardChance percent of C2 packs bring something that puts fire on the ground. C3's packs are
        /// SurviveSize strong at SurviveLevels above C1's for the rung: meant to be able to kill.
        ///
        /// **Placement**: the seat starts at one of the dungeon's own creature spawn points (its corridors, cleared),
        /// within CorridorWalk yards' walk of the entrance and CorridorSpacing apart; a creature or pack stands
        /// FightNearest to FightFurthest yards from the seat, in its line of sight; the next pack NextNearest to
        /// NextFurthest on from the current one, further from the seat. C1's next creature comes NextFightMs after a
        /// kill. **Death** (I4, EntranceRespawn): Respawn.DelayMs out, then alive at the entrance; back once within
        /// Respawn.RejoinYards of the fight (ArenaDefinition::RespawnAtEntrance).
        struct CombatTuning
        {
            float Kill = 1.0f;
            float Clear = 2.0f;
            float Survived = 1.0f;
            float SurviveSurvived = 2.0f;
            float InterruptLanded = 0.25f;
            float Away = 0.02f;                 // per second dead or away from the fight
            float AwayYards = 30.0f;
            float Death = 2.0f;
            float AllyDeath = 1.0f;
            float Hurt = 0.2f;
            float FireHurt = 1.0f;
            float ExtraPull = 1.0f;
            float Clock = 0.01f;                // per second
            float Damage = 0.3f;                // shaping, per creature health
            uint32 MaxTier = 5;
            int32 LevelBase = -2;
            uint32 LevelsPerTier = 1;
            uint32 EliteTier = 4;
            uint32 CasterTier = 1;
            uint32 LinkedTier = 2;
            int32 HazardChance = 33;
            uint32 SurviveSize = 3;
            uint32 SurviveLevels = 2;
            float FightNearest = 28.0f;
            float FightFurthest = 40.0f;
            float NextNearest = 30.0f;
            float NextFurthest = 45.0f;
            uint32 NextFightMs = 2000;
            float CorridorWalk = 220.0f;
            float CorridorSpacing = 8.0f;
        } Combat;

        /// **The roles stage** (Opposition::Roles, RolesEncounter; dungeon-curriculum G1): a party of five on the
        /// combat stages' cleared Ragefire Chasm, one role drilled an episode -- the drilled role's seat is seat 0
        /// (StageScenario's DrillRole makeup: its class and build drawn among those whose spec plays it), the others a
        /// proper party around it, every seat learned or a partner (I7).
        ///
        /// **Outcome**, times the rung's w = 1 + Difficulty.TierScale x tier (the archived ladders' tier scale):
        /// Clear for each pack the party clears (every seat); the drilled seat's lesson, paid only to it --
        /// DrillHold (tank_hold: Hold per enemy on it, a decision), DrillKeep (heal_keep: Keep per party member above
        /// half health, a decision), DrillFocus (damage_discipline: Focus per share of the damage scale it put on the
        /// tank's target), PullClean (pull: a pack cleared with no other pack in the fight; the others PullOthers of
        /// it); Survived at the episode's end with no death of the seat's own. **Cost**, divided by w where it is a
        /// loss: the drilled seat's misses (Loose per enemy on someone else; PulledOff per enemy it took off the tank,
        /// a decision; KeepLow per member under 35% or dead, a decision; Overheal of what its heals wasted), PullExtra
        /// for a second pack drawn into a fight (the pull drill's puller in full, the others PullOthers of it), Death,
        /// Away per second dead or away from the fight (beyond AwayYards while it fights, or walking back), and Clock
        /// per second a pack fights. **Shaping** (faded): Damage per share of a creature's health dealt.
        ///
        /// **The ladder** (DifficultyLadder, per class and build of the drilled seat, on its own window): rung t of
        /// MaxTier puts the packs at the party's level + LevelBase + t x LevelsPerTier / 2, PackSizeFirst creatures
        /// growing by one every PackGrowEvery rungs up to PackSizeMax, a caster from CasterTier, linked from LinkedTier,
        /// an elite from EliteTier; heal_keep's packs at KeepHealthPct of their health (a fight longer than a mana bar);
        /// the pull drill's camp CampPacksFirst packs (one more every two rungs, CampPacksMax at most) standing
        /// CampSpacingFirst yards apart at rung 0, closing to CampSpacingLast at the top.
        ///
        /// **Placement**: the party starts round one of the dungeon's corridor points (the combat stages' Combat.
        /// CorridorWalk and CorridorSpacing), its members PartyNearest-PartyFurthest yards from seat 0 in its sight; the
        /// first pack FightNearest-FightFurthest (Combat.*) from seat 0 in its sight, the next ones further on.
        /// **Won** (the ladder's window and the evaluation's `won`): a pack cleared, no wipe, and the drill's measure:
        /// the tank holding WinHold of the enemy-decisions; no party member dead; the damage dealer's damage WinFocus on
        /// the tank's target with at most WinPulledSeconds of enemies taken off it; no second pack in a fight.
        /// **Death** (I4): Respawn.DelayMs out, then alive at the entrance, walking back. StandInShare percent of the
        /// training episodes put the "human" stand-in (StandIn.*: a frozen learned partner) in one seat other than the
        /// drilled one.
        struct RolesTuning
        {
            float Clear = 1.0f;
            float Hold = 0.045f;                // per enemy on the drilled tank, a decision (3 x Raid.TankHold)
            float Loose = 0.018f;               // per enemy on somebody else
            float Focus = 0.9f;                 // per damage-scale share on the tank's target (3 x Raid.TankTarget)
            float PulledOff = 0.012f;           // per enemy on the damage dealer, a decision
            float Keep = 0.0006f;               // per member above half health, a decision (3 x Raid.KeepUp)
            float KeepLow = 0.0006f;            // per member below 35% or dead, a decision
            float Overheal = 0.5f;              // per maximum health of healing wasted, x Party.TeammateHealing
            float PullClean = 2.0f;
            float PullExtra = 1.5f;
            float PullOthers = 0.5f;            // the other seats' share of PullClean and PullExtra
            float Survived = 1.0f;
            float Death = 2.0f;
            float Away = 0.02f;                 // per second
            float AwayYards = 30.0f;
            float Clock = 0.01f;                // per second a pack fights
            float Damage = 0.3f;                // shaping, per creature health
            uint32 MaxTier = 5;
            int32 LevelBase = -1;
            uint32 LevelsPerTier = 1;
            uint32 PackSizeFirst = 2;
            uint32 PackGrowEvery = 2;
            uint32 PackSizeMax = 4;
            uint32 CasterTier = 1;
            uint32 LinkedTier = 2;
            uint32 EliteTier = 4;
            uint32 KeepHealthPct = 200;
            uint32 CampPacksFirst = 2;
            uint32 CampPacksMax = 4;
            float CampSpacingFirst = 45.0f;
            float CampSpacingLast = 25.0f;
            float PartyNearest = 2.0f;
            float PartyFurthest = 6.0f;
            float WinHold = 0.75f;
            float WinFocus = 0.6f;
            float WinPulledSeconds = 5.0f;
            int32 StandInShare = 20;            // percent of training episodes
        } Roles;

        /// Resurrecting: a seat's own Soulstone or Reincarnation, and revives on allies (companion and party stages).
        struct ResurrectionTuning
        {
            uint32 GraceMs = 20000;             // the dead wait this long for a resurrection they can get before solo
                                                // stages end and owner stages stand them up (the next pull waits too)
            float ReviveAlly = 1.5f;            // a dead ally the seat resurrected stood up
        } Resurrection;

        /// Output: how fast the enemies die. Damage is paid as a share of their health, so a kill paid the same
        /// whether it took ten seconds or thirty, and only discounting and a small FastKill told a seat to hurry.
        struct OutputTuning
        {
            /// Per second an engaged enemy lives, charged to every seat of the side -- a dead one too, so dying
            /// never stops it -- in every arena but a raid's, whose wipe would. At 0.03 a 30 s fight costs 0.9,
            /// and doubling the seat's damage saves half of it.
            float Clock = 0.03f;
        } Output;

        /// The "human" stand-in seat of the party stages (StandIn.h: a frozen learned partner in one seat, its style
        /// leading or following, in the role it wants). Off unless StandIn.Share (or an arena's own share) is set.
        StandIn::Tuning StandIn;

        /// Calls f(key, value) for every value, key relative to the tuning prefix (AnimusForge.Curriculum., ...).
        template <typename T, typename F>
        static void Visit(T& tuning, F&& f)
        {
            f("Characters.HighLevelFirst", tuning.Characters.HighLevelFirst);
            f("Characters.HighLevelChance", tuning.Characters.HighLevelChance);
            f("Characters.LowLevelLast", tuning.Characters.LowLevelLast);
            f("Characters.LowLevelChance", tuning.Characters.LowLevelChance);
            f("Characters.NoisyTalentChance", tuning.Characters.NoisyTalentChance);
            f("Characters.RandomTalentChance", tuning.Characters.RandomTalentChance);
            f("Characters.TalentNoisePoints", tuning.Characters.TalentNoisePoints);
            f("Characters.PetOutChance", tuning.Characters.PetOutChance);
            f("Characters.ReuseEpisodes", tuning.Characters.ReuseEpisodes);
            f("Characters.KeepCasting", tuning.Characters.KeepCasting);

            f("Party.ClassicChance", tuning.Party.ClassicChance);
            f("Party.RoleTankChance", tuning.Party.RoleTankChance);
            f("Party.RoleHealerChance", tuning.Party.RoleHealerChance);
            f("Party.TeammateDamageTakenDps", tuning.Party.TeammateDamageTakenDps);
            f("Party.TeammateDamageTakenProtector", tuning.Party.TeammateDamageTakenProtector);
            f("Party.TeammateHealing", tuning.Party.TeammateHealing);
            f("Party.HealOffGoal", tuning.Party.HealOffGoal);
            f("Party.TankDamageShare", tuning.Party.TankDamageShare);
            f("Party.TankLoseTeammate", tuning.Party.TankLoseTeammate);
            f("Party.PulledThreat", tuning.Party.PulledThreat);
            f("Party.TeammateDeath", tuning.Party.TeammateDeath);
            f("Raid.TankHold", tuning.Raid.TankHold);
            f("Raid.TankLoose", tuning.Raid.TankLoose);
            f("Raid.TankTarget", tuning.Raid.TankTarget);
            f("Raid.PulledOff", tuning.Raid.PulledOff);
            f("Raid.EarlyPull", tuning.Raid.EarlyPull);
            f("Raid.KeepUp", tuning.Raid.KeepUp);
            f("Raid.Overheal", tuning.Raid.Overheal);
            f("Raid.TankStance", tuning.Raid.TankStance);
            f("Raid.Idle", tuning.Raid.Idle);
            f("Raid.IdleMs", tuning.Raid.IdleMs);
            f("Raid.IdleReach", tuning.Raid.IdleReach);
            f("Duel.MeleeRange", tuning.Duel.MeleeRange);
            f("Duel.RangedRange", tuning.Duel.RangedRange);
            f("Difficulty.RaiseAbove", tuning.Difficulty.RaiseAbove);
            f("Difficulty.LowerBelow", tuning.Difficulty.LowerBelow);
            f("Difficulty.Window", tuning.Difficulty.Window);
            f("Difficulty.ReviewChance", tuning.Difficulty.ReviewChance);
            f("Difficulty.StretchChance", tuning.Difficulty.StretchChance);
            f("Difficulty.CasterChance", tuning.Difficulty.CasterChance);
            f("Difficulty.TierScale", tuning.Difficulty.TierScale);
            f("Instance.MaxTierScale", tuning.Instance.MaxTierScale);
            f("Instance.WingTrashKill", tuning.Instance.WingTrashKill);
            f("Instance.WingBoss", tuning.Instance.WingBoss);
            f("Instance.WingMidBoss", tuning.Instance.WingMidBoss);
            f("Instance.WingDeath", tuning.Instance.WingDeath);
            f("Instance.WingWipe", tuning.Instance.WingWipe);
            f("Instance.WingWipes", tuning.Instance.WingWipes);
            f("Instance.WingStall", tuning.Instance.WingStall);
            f("Instance.WingStallOthers", tuning.Instance.WingStallOthers);
            f("Instance.WingEngage", tuning.Instance.WingEngage);
            f("Instance.WingReadyShare", tuning.Instance.WingReadyShare);
            f("Instance.WingStallGraceMs", tuning.Instance.WingStallGraceMs);
            f("Instance.WingTimeout", tuning.Instance.WingTimeout);
            f("Instance.WingClear", tuning.Instance.WingClear);
            f("Instance.WingClock", tuning.Instance.WingClock);
            f("Instance.WingProbe", tuning.Instance.WingProbe);
            f("Instance.WingRungRuns", tuning.Instance.WingRungRuns);
            f("Instance.WingRungTarget", tuning.Instance.WingRungTarget);
            f("Instance.WingRungStart", tuning.Instance.WingRungStart);
            f("Instance.WingSupplies", tuning.Instance.WingSupplies);
            f("Instance.WingTrace", tuning.Instance.WingTrace);
            f("Instance.WingCrowd", tuning.Instance.WingCrowd);
            f("Instance.WingCrowdFree", tuning.Instance.WingCrowdFree);
            f("Instance.WingStray", tuning.Instance.WingStray);
            f("Instance.WingAway", tuning.Instance.WingAway);
            f("Instance.WingStrayYards", tuning.Instance.WingStrayYards);

            f("Actions.RepeatMs", tuning.Actions.RepeatMs);
            f("Actions.MoveRepeatMs", tuning.Actions.MoveRepeatMs);
            f("Actions.StopCastMinMs", tuning.Actions.StopCastMinMs);
            f("Actions.RecastAfterStopMs", tuning.Actions.RecastAfterStopMs);
            f("Actions.ModeLockMs", tuning.Actions.ModeLockMs);
            f("Actions.Repeat", tuning.Actions.Repeat);
            f("Actions.RepeatWindowMs", tuning.Actions.RepeatWindowMs);
            f("Actions.RepeatFree", tuning.Actions.RepeatFree);
            f("Actions.Jitter", tuning.Actions.Jitter);
            f("Actions.Aimless", tuning.Actions.Aimless);
            f("Actions.Aimless.OffFocus", tuning.Actions.AimlessOffFocus);
            f("Actions.Aimless.AoeMissed", tuning.Actions.AimlessAoeMissed);
            f("Actions.Aimless.InRangeCast", tuning.Actions.AimlessInRangeCast);
            f("Actions.Aimless.UnprovokedHarm", tuning.Actions.AimlessUnprovokedHarm);
            f("Actions.Aimless.HelpOffGoal", tuning.Actions.AimlessHelpOffGoal);
            f("Actions.Aimless.StepAway", tuning.Actions.AimlessStepAway);
            f("Actions.Aimless.TargetSwitch", tuning.Actions.AimlessTargetSwitch);
            f("Actions.Aimless.PetOffGoal", tuning.Actions.AimlessPetOffGoal);
            f("Actions.Aimless.ConsumeNotNeeded", tuning.Actions.AimlessConsumeNotNeeded);
            f("Actions.Aimless.TrapNoEnemy", tuning.Actions.AimlessTrapNoEnemy);
            f("Actions.Aimless.ModeFlip", tuning.Actions.AimlessModeFlip);
            f("Actions.Aimless.ModeReverse", tuning.Actions.AimlessModeReverse);
            f("Actions.Aimless.NeedlessMove", tuning.Actions.AimlessNeedlessMove);
            f("Actions.Aimless.TauntOffRole", tuning.Actions.AimlessTauntOffRole);
            f("Actions.Aimless.TankModeOffRole", tuning.Actions.AimlessTankModeOffRole);
            f("Actions.Aimless.CastFailed", tuning.Actions.AimlessCastFailed);
            f("Actions.Aimless.ActRefused", tuning.Actions.AimlessActRefused);
            f("Actions.ModeSwitch", tuning.Actions.ModeSwitch);
            f("Actions.SupplySpent", tuning.Actions.SupplySpent);
            f("Actions.ConsumeFullPct", tuning.Actions.ConsumeFullPct);
            f("Actions.Effort", tuning.Actions.Effort);
            f("Actions.Fidget", tuning.Actions.Fidget);
            f("Actions.SettleGraceMs", tuning.Actions.SettleGraceMs);
            f("Actions.IntentSlackYards", tuning.Actions.IntentSlackYards);

            f("Goals.Reached", tuning.Goals.Reached);
            f("Goals.Switch", tuning.Goals.Switch);
            f("Goals.Progress", tuning.Goals.Progress);
            f("Goals.ProgressGamma", tuning.Goals.ProgressGamma);
            f("Goals.FightValue", tuning.Goals.FightValue);
            f("Goals.ControlValue", tuning.Goals.ControlValue);
            f("Goals.RecoverValue", tuning.Goals.RecoverValue);
            f("Goals.ProtectValue", tuning.Goals.ProtectValue);
            f("Goals.ProtectHoldMs", tuning.Goals.ProtectHoldMs);
            f("Goals.TravelValue", tuning.Goals.TravelValue);
            f("Goals.SecondaryShare", tuning.Goals.SecondaryShare);
            f("Goals.Secondary", tuning.Goals.Secondary);

            f("Support.SelfHealing", tuning.Support.SelfHealing);
            f("Support.HealingMana", tuning.Support.HealingMana);

            f("Options.RestMaxMs", tuning.Options.RestMaxMs);
            f("Options.HoldInterruptMs", tuning.Options.HoldInterruptMs);
            f("Hazards.Damage", tuning.Hazards.Damage);
            f("Hazards.Standing", tuning.Hazards.Standing);
            f("Hazards.Max", tuning.Hazards.Max);
            f("Markers.Arrive", tuning.Markers.Arrive);
            f("Markers.StepCost", tuning.Markers.StepCost);
            f("Markers.Death", tuning.Markers.Death);
            f("Markers.Progress", tuning.Markers.Progress);
            f("Markers.Facing", tuning.Markers.Facing);
            f("Markers.StopMoved", tuning.Markers.StopMoved);
            f("Markers.StopNear", tuning.Markers.StopNear);
            f("Markers.ArriveRise", tuning.Markers.ArriveRise);
            f("Respawn.DelayMs", tuning.Respawn.DelayMs);
            f("Respawn.RejoinYards", tuning.Respawn.RejoinYards);
            f("PartyFollow.BandMin", tuning.PartyFollow.BandMin);
            f("PartyFollow.BandMax", tuning.PartyFollow.BandMax);
            f("PartyFollow.LostYards", tuning.PartyFollow.LostYards);
            f("PartyFollow.Kept", tuning.PartyFollow.Kept);
            f("PartyFollow.Lost", tuning.PartyFollow.Lost);
            f("PartyFollow.Regroup", tuning.PartyFollow.Regroup);
            f("PartyFollow.RegroupWindow", tuning.PartyFollow.RegroupWindow);
            f("PartyFollow.RegroupMinStopMs", tuning.PartyFollow.RegroupMinStopMs);
            f("PartyFollow.Blocking", tuning.PartyFollow.Blocking);
            f("PartyFollow.BlockYards", tuning.PartyFollow.BlockYards);
            f("PartyFollow.BlockHalfAngle", tuning.PartyFollow.BlockHalfAngle);
            f("PartyFollow.Death", tuning.PartyFollow.Death);
            f("PartyFollow.MinimapYards", tuning.PartyFollow.MinimapYards);
            f("PartyFollow.WalkRungs", tuning.PartyFollow.WalkRungs);
            f("PartyFollow.SuddenFromRung", tuning.PartyFollow.SuddenFromRung);
            f("PartyFollow.BackStepFromRung", tuning.PartyFollow.BackStepFromRung);
            f("PartyFollow.StopSecondsFirst", tuning.PartyFollow.StopSecondsFirst);
            f("PartyFollow.StopSecondsLast", tuning.PartyFollow.StopSecondsLast);
            f("PartyFollow.SuddenStopMinMs", tuning.PartyFollow.SuddenStopMinMs);
            f("PartyFollow.SuddenStopMaxMs", tuning.PartyFollow.SuddenStopMaxMs);
            f("PartyFollow.SuddenGapMin", tuning.PartyFollow.SuddenGapMin);
            f("PartyFollow.SuddenGapMax", tuning.PartyFollow.SuddenGapMax);
            f("PartyFollow.BackStepYards", tuning.PartyFollow.BackStepYards);
            f("PartyFollow.BackStepChance", tuning.PartyFollow.BackStepChance);
            f("PartyFollow.GiveUpMs", tuning.PartyFollow.GiveUpMs);
            f("PartyFollow.CastShare", tuning.PartyFollow.CastShare);
            f("Seek.Arrive", tuning.Seek.Arrive);
            f("Seek.StepCost", tuning.Seek.StepCost);
            f("Seek.Death", tuning.Seek.Death);
            f("Seek.ArriveRise", tuning.Seek.ArriveRise);
            f("Seek.Sighting", tuning.Seek.Sighting);
            f("Seek.NewGround", tuning.Seek.NewGround);
            f("Seek.NewGroundCell", tuning.Seek.NewGroundCell);
            f("Seek.Stuck", tuning.Seek.Stuck);
            f("Seek.Wall", tuning.Seek.Wall);
            f("Seek.WallSlide", tuning.Seek.WallSlide);
            f("Seek.Attempts", tuning.Seek.Attempts);
            f("Seek.FloorTolerance", tuning.Seek.FloorTolerance);
            f("Seek.Clearance", tuning.Seek.Clearance);
            f("Seek.RoomSeen", tuning.Seek.RoomSeen);
            f("Seek.RoomSeenRays", tuning.Seek.RoomSeenRays);
            f("Seek.CarryShare", tuning.Seek.CarryShare);
            f("Seek.RungSeconds0", tuning.Seek.RungSeconds0);
            f("Seek.RungSeconds1", tuning.Seek.RungSeconds1);
            f("Seek.RungSeconds2", tuning.Seek.RungSeconds2);
            f("Seek.RungSeconds3", tuning.Seek.RungSeconds3);
            f("Seek.HallwayNearest", tuning.Seek.HallwayNearest);
            f("Seek.HallwayFurthest", tuning.Seek.HallwayFurthest);
            f("Seek.DoorwayInside", tuning.Seek.DoorwayInside);
            f("Seek.DoorwayDeeper", tuning.Seek.DoorwayDeeper);
            f("Seek.DoorwaySpread", tuning.Seek.DoorwaySpread);
            f("Seek.Goals", tuning.Seek.Goals);
            f("Seek.GoalsFromRung", tuning.Seek.GoalsFromRung);
            f("Seek.RoomGoal", tuning.Seek.RoomGoal);
            f("Seek.RoomSwitch", tuning.Seek.RoomSwitch);
            f("Seek.Return", tuning.Seek.Return);
            f("Seek.AidUntil", tuning.Seek.AidUntil);
            f("Seek.GlimpseRays", tuning.Seek.GlimpseRays);
            f("Seek.CheckedShare", tuning.Seek.CheckedShare);
            f("Seek.EnterDwellMs", tuning.Seek.EnterDwellMs);
            f("Seek.ReturnAwayYards", tuning.Seek.ReturnAwayYards);
            f("Seek.ReturnAwayMs", tuning.Seek.ReturnAwayMs);
            f("Seek.GoalSource", tuning.Seek.GoalSource);
            f("Seek.CellReach", tuning.Seek.CellReach);
            f("Seek.CellRise", tuning.Seek.CellRise);
            f("Seek.CellSame", tuning.Seek.CellSame);
            f("Seek.CellMinYards", tuning.Seek.CellMinYards);
            f("Seek.CellPatienceMs", tuning.Seek.CellPatienceMs);
            f("Seek.CellPatienceYards", tuning.Seek.CellPatienceYards);
            f("Seek.CellGoal", tuning.Seek.CellGoal);
            f("Seek.CellProgress", tuning.Seek.CellProgress);
            f("Seek.CellSwitch", tuning.Seek.CellSwitch);
            f("Seek.CellLost", tuning.Seek.CellLost);
            f("Seek.CellStale", tuning.Seek.CellStale);
            f("Seek.ExploreSeen", tuning.Seek.ExploreSeen);
            f("Seek.ExploreCap", tuning.Seek.ExploreCap);
            f("Seek.ExploreFloor", tuning.Seek.ExploreFloor);
            f("Seek.ExploreRoomBonus", tuning.Seek.ExploreRoomBonus);
            f("Seek.FrontierPull", tuning.Seek.FrontierPull);
            f("Seek.FrontierCap", tuning.Seek.FrontierCap);
            f("Seek.CircleWindowMs", tuning.Seek.CircleWindowMs);
            f("Seek.CircleYards", tuning.Seek.CircleYards);
            f("Seek.CircleNetYards", tuning.Seek.CircleNetYards);
            f("Seek.CircleTurnDeg", tuning.Seek.CircleTurnDeg);
            f("Seek.Circling", tuning.Seek.Circling);
            f("Seek.TrapShare", tuning.Seek.TrapShare);
            f("Seek.Escape", tuning.Seek.Escape);
            f("Seek.TrapEscapeYards", tuning.Seek.TrapEscapeYards);
            f("Seek.TrapEscapeMs", tuning.Seek.TrapEscapeMs);
            f("Interact.Arrive", tuning.Interact.Arrive);
            f("Interact.DoorOpened", tuning.Interact.DoorOpened);
            f("Interact.WrongObject", tuning.Interact.WrongObject);
            f("Interact.StepCost", tuning.Interact.StepCost);
            f("Interact.Death", tuning.Interact.Death);
            f("Interact.ArriveRise", tuning.Interact.ArriveRise);
            f("Interact.Sighting", tuning.Interact.Sighting);
            f("Interact.Stuck", tuning.Interact.Stuck);
            f("Interact.Wall", tuning.Interact.Wall);
            f("Interact.WallSlide", tuning.Interact.WallSlide);
            f("Interact.CarryShare", tuning.Interact.CarryShare);
            f("Interact.RungSeconds0", tuning.Interact.RungSeconds0);
            f("Interact.RungSeconds1", tuning.Interact.RungSeconds1);
            f("Interact.RungSeconds2", tuning.Interact.RungSeconds2);
            f("Interact.DecoysMin", tuning.Interact.DecoysMin);
            f("Interact.DecoysMax", tuning.Interact.DecoysMax);
            f("Interact.Spacing", tuning.Interact.Spacing);
            f("Interact.SightNearest", tuning.Interact.SightNearest);
            f("Interact.SightFurthest", tuning.Interact.SightFurthest);
            f("Interact.Attempts", tuning.Interact.Attempts);
            f("Interact.SummonSweep", tuning.Interact.SummonSweep);
            f("Controls.Nearest", tuning.Controls.Nearest);
            f("Controls.Furthest", tuning.Controls.Furthest);
            f("Controls.ArriveTolerance", tuning.Controls.ArriveTolerance);
            f("Controls.CornerShare", tuning.Controls.CornerShare);
            f("Controls.CornerFrom", tuning.Controls.CornerFrom);
            f("Controls.CornerStep", tuning.Controls.CornerStep);
            f("Controls.Attempts", tuning.Controls.Attempts);
            f("Controls.Withhold0", tuning.Controls.Withhold0);
            f("Controls.Withhold1", tuning.Controls.Withhold1);
            f("Controls.Withhold2", tuning.Controls.Withhold2);
            f("Controls.Withhold3", tuning.Controls.Withhold3);
            f("Controls.Stuck", tuning.Controls.Stuck);
            f("Controls.Wall", tuning.Controls.Wall);
            f("Controls.WallSlide", tuning.Controls.WallSlide);
            f("Combat.Kill", tuning.Combat.Kill);
            f("Combat.Clear", tuning.Combat.Clear);
            f("Combat.Survived", tuning.Combat.Survived);
            f("Combat.SurviveSurvived", tuning.Combat.SurviveSurvived);
            f("Combat.InterruptLanded", tuning.Combat.InterruptLanded);
            f("Combat.Away", tuning.Combat.Away);
            f("Combat.AwayYards", tuning.Combat.AwayYards);
            f("Combat.Death", tuning.Combat.Death);
            f("Combat.AllyDeath", tuning.Combat.AllyDeath);
            f("Combat.Hurt", tuning.Combat.Hurt);
            f("Combat.FireHurt", tuning.Combat.FireHurt);
            f("Combat.ExtraPull", tuning.Combat.ExtraPull);
            f("Combat.Clock", tuning.Combat.Clock);
            f("Combat.Damage", tuning.Combat.Damage);
            f("Combat.MaxTier", tuning.Combat.MaxTier);
            f("Combat.LevelBase", tuning.Combat.LevelBase);
            f("Combat.LevelsPerTier", tuning.Combat.LevelsPerTier);
            f("Combat.EliteTier", tuning.Combat.EliteTier);
            f("Combat.CasterTier", tuning.Combat.CasterTier);
            f("Combat.LinkedTier", tuning.Combat.LinkedTier);
            f("Combat.HazardChance", tuning.Combat.HazardChance);
            f("Combat.SurviveSize", tuning.Combat.SurviveSize);
            f("Combat.SurviveLevels", tuning.Combat.SurviveLevels);
            f("Combat.FightNearest", tuning.Combat.FightNearest);
            f("Combat.FightFurthest", tuning.Combat.FightFurthest);
            f("Combat.NextNearest", tuning.Combat.NextNearest);
            f("Combat.NextFurthest", tuning.Combat.NextFurthest);
            f("Combat.NextFightMs", tuning.Combat.NextFightMs);
            f("Combat.CorridorWalk", tuning.Combat.CorridorWalk);
            f("Combat.CorridorSpacing", tuning.Combat.CorridorSpacing);
            f("Roles.Clear", tuning.Roles.Clear);
            f("Roles.Hold", tuning.Roles.Hold);
            f("Roles.Loose", tuning.Roles.Loose);
            f("Roles.Focus", tuning.Roles.Focus);
            f("Roles.PulledOff", tuning.Roles.PulledOff);
            f("Roles.Keep", tuning.Roles.Keep);
            f("Roles.KeepLow", tuning.Roles.KeepLow);
            f("Roles.Overheal", tuning.Roles.Overheal);
            f("Roles.PullClean", tuning.Roles.PullClean);
            f("Roles.PullExtra", tuning.Roles.PullExtra);
            f("Roles.PullOthers", tuning.Roles.PullOthers);
            f("Roles.Survived", tuning.Roles.Survived);
            f("Roles.Death", tuning.Roles.Death);
            f("Roles.Away", tuning.Roles.Away);
            f("Roles.AwayYards", tuning.Roles.AwayYards);
            f("Roles.Clock", tuning.Roles.Clock);
            f("Roles.Damage", tuning.Roles.Damage);
            f("Roles.MaxTier", tuning.Roles.MaxTier);
            f("Roles.LevelBase", tuning.Roles.LevelBase);
            f("Roles.LevelsPerTier", tuning.Roles.LevelsPerTier);
            f("Roles.PackSizeFirst", tuning.Roles.PackSizeFirst);
            f("Roles.PackGrowEvery", tuning.Roles.PackGrowEvery);
            f("Roles.PackSizeMax", tuning.Roles.PackSizeMax);
            f("Roles.CasterTier", tuning.Roles.CasterTier);
            f("Roles.LinkedTier", tuning.Roles.LinkedTier);
            f("Roles.EliteTier", tuning.Roles.EliteTier);
            f("Roles.KeepHealthPct", tuning.Roles.KeepHealthPct);
            f("Roles.CampPacksFirst", tuning.Roles.CampPacksFirst);
            f("Roles.CampPacksMax", tuning.Roles.CampPacksMax);
            f("Roles.CampSpacingFirst", tuning.Roles.CampSpacingFirst);
            f("Roles.CampSpacingLast", tuning.Roles.CampSpacingLast);
            f("Roles.PartyNearest", tuning.Roles.PartyNearest);
            f("Roles.PartyFurthest", tuning.Roles.PartyFurthest);
            f("Roles.WinHold", tuning.Roles.WinHold);
            f("Roles.WinFocus", tuning.Roles.WinFocus);
            f("Roles.WinPulledSeconds", tuning.Roles.WinPulledSeconds);
            f("Roles.StandInShare", tuning.Roles.StandInShare);
            f("Options.JitterDecayMs", tuning.Options.JitterDecayMs);

            f("Resurrection.GraceMs", tuning.Resurrection.GraceMs);
            f("Resurrection.ReviveAlly", tuning.Resurrection.ReviveAlly);
            f("Output.Clock", tuning.Output.Clock);

            f("StandIn.Share", tuning.StandIn.Share);
            f("StandIn.LeadChance", tuning.StandIn.LeadChance);
            f("StandIn.TankChance", tuning.StandIn.TankChance);
            f("StandIn.HealerChance", tuning.StandIn.HealerChance);
        }

        /// The values of the config keys <prefix><key>, each defaulting to the value above; min/max pairs are
        /// ordered.
        [[nodiscard]] static CurriculumTuning Load(std::string const& prefix);

        /// Every value as a JSON object, keys as in the config.
        [[nodiscard]] boost::json::object Json() const;
    };
}

#endif
