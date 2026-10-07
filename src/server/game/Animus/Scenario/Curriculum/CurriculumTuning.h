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
#include "StandIn.h"
#include <boost/json/fwd.hpp>
#include <string>

/*
 * Every value that shapes what the curriculum trains on and what it is paid for: the characters, parties,
 * pulls and scripted players it meets, and the reward weights. Each is the config key <prefix><key>, the prefix being
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
            int32 SizeWeight1 = 20;             // relative chance of 1, 2, 3 or 4 seats with a character
            int32 SizeWeight2 = 20;
            int32 SizeWeight3 = 20;
            int32 SizeWeight4 = 40;
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
            float HealthPerGroup = 1.0f;        // a raid pull's health x (seats / 5) x this
            float DamagePerGroup = 0.15f;       // ... and its melee damage x (1 + this x (groups - 1))
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
            /// A drill's drilled seat (ArenaDefinition::DrillRole): its role's terms times this.
            float DrillWeight = 3.0f;
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
            /// Damage dealers in a raid: their own damage as a share of the level's damage scale, times this.
            float Output = 0.5f;
            /// Party and raid: per decision in a fight with an enemy in reach, once nothing the seat did served or
            /// was neutral -- no press, no damage, no healing -- for IdleMs. Per 50 ms of tuning: a minute idle
            /// costs about 1.2.
            float Idle = 0.001f;
            uint32 IdleMs = 4000;
            float IdleReach = 40.0f;            // yards: an enemy this near is one to act on
        } Raid;

        /// One-on-one fights against a creature (the duel) or a player (PvP).
        struct DuelTuning
        {
            float DamageDealt = 2.0f;           // fraction of the opponent's health: a kill is worth this in damage
            float DamageTaken = 1.0f;           // fraction of the bot's health
            float Approach = 0.5f;              // shaping toward the spec's range, per 40 yd closed
            float StealthOpener = 0.5f;         // a harmful spell from stealth that breaks it (Ambush, Cheap Shot,
                                                // a feral druid's Pounce or Ravage out of Prowl)
            float StealthUtility = 0.05f;       // one that keeps it (Sap, Distract), once per target per stealth
            float StepCost = 0.0002f;           // per decision
            /// Winning is what the stage is for, so the kill and the losses dwarf the rest. With Kill 3, FastKill up to
            /// 3 and a loss at -3, a risky fast opener (time bonus ~2.4) beat a sure slow win (~0.6) as soon as it won
            /// 79% of the time: the reward traded one fight in five for speed. At Kill 10, FastKill 1 and a loss at
            /// -10, that break-even is ~97%. The dense terms (damage, approach) stay small, as guidance.
            float Kill = 10.0f;
            float FastKill = 1.0f;              // times the fraction of the episode length left, from the engagement
            /// Times the fraction of the bot's health not lost. Damage taken is already charged as it happens
            /// (DamageTaken, dense), so this pays for the same thing again at the kill; together they were
            /// worth three times the damage dealt term, which reads as "survive" more than "win". stage1_duel
            /// bore that out: the policy beat the baseline on score everywhere while killing less often than it
            /// did as a rogue (0.87 against 0.94) and below level 20 (0.78 against 0.83), banking the difference
            /// in health it never spent. Halved, against a larger Kill, so that winning the fight outweighs
            /// finishing it untouched -- deaths were 0.002 an episode, so there is room to push.
            float HealthKept = 0.5f;
            float Death = 10.0f;
            /// A creature duel that runs out the clock without a kill (and without a death, which Death already
            /// charges). The duel is won by killing, so a timeout is a lost fight and ends the episode as one, not
            /// a cut-off the critic bootstraps across: without it the cheapest fight to lose was the one never
            /// started (stage1_duel at 30M: none of the 11 failed warlock episodes took any damage). As Death, so
            /// neither way of losing is the cheaper one to learn.
            float Timeout = 10.0f;
            /// The share of Timeout charged whatever the fight's progress; the rest is charged times the share of the
            /// opponent still standing, so a near miss costs about half a refusal. Without the floor, breaking off
            /// was cheaper than dying: the fights lost to the clock on 2026-09-17 ended with 71% of the opponent
            /// left, which is -6.7 unfloored against -8.9 for dying and -9.5 for the flat charge it replaced.
            float TimeoutFloor = 0.5f;
            /// Creature duel: per second the fight has not started once StallGraceMs of the episode are gone. Timeout
            /// alone charges standing still only at the end of the clock, 900 decisions away: stage1_duel at 20M had
            /// its deterministic policy stand where it spawned for all 90 s in 67 of 2048 episodes (21 without a
            /// single action), fights the same policy sampled won. Raised from 0.05 on 2026-09-17: standing at range
            /// held SeatGoal::Position and earned Goals.Match at 0.01 a decision against this at 0.0125, so keeping
            /// out of the fight was all but free -- 24 of the 87 elite fights lost to the clock were never engaged.
            float Stall = 0.08f;
            uint32 StallGraceMs = 15000;        // summoning a pet, buffing and sneaking up in stealth fit in this
            /// Preparing is not stalling: the grace grows by the time the seat spent starting buffs, forms, stances,
            /// stealth and pet summons out of combat (CombatTally::PreparationMs), up to this much. 30 s made a 45 s
            /// approach free; buffing, a form, a pet and an opener from stealth fit in 15.
            uint32 PreparationRefundMaxMs = 15000;
            /// Creature duel, ranged specs: per second the opponent stands in melee range hitting the bot. The approach
            /// shaping only pays for closing in, so nothing told a hunter, mage or warlock to keep the range it
            /// fights best at (stage1_duel at 20M: 88 of 96 hunter kills ended within 5 yd).
            /// Stopping the opponent's cast. The duel meets casters now (Difficulty.CasterChance), so this is the
            /// cheapest place to learn what an interrupt is for: one enemy, one cast, nothing else happening. Priced
            /// by what was stopped, as the pack's is.
            float Interrupt = 0.3f;
            float InterruptHeal = 3.0f;
            float InterruptArea = 2.0f;
            float InterruptLong = 1.5f;
            /// 0.03 cost a mage about 0.4 a fight against 15 for a kill, and mages spent 60-75% of their duels in melee
            /// range, dying to elites they let close (2026-10-02, stage4 at 160M); 0.1 puts 15 s in melee at 1.5.
            float Spacing = 0.1f;
            /// A ranged spec's shots (Auto Shot, Steady Shot, a wand) landed while nothing hits it in melee reach, as a
            /// share of the fight's enemy health, on top of DamageDealt: shooting from range is worth more than the
            /// same damage in melee. Duel and pack. Hunters let packs close and fought half of each pull in melee
            /// (2026-10-02, stage5 at 60M): Spacing alone was outweighed by finishing sooner.
            float ShotAtRange = 1.0f;
            /// Per second a seat with Auto Shot (or Shoot) running moves while its target is alive and in range and
            /// nothing hits it in melee reach and nothing on the ground hurts it: moving stops the shots.
            float ShotPaused = 0.05f;
            /// The damage the seat's pet takes, as a share of the seat's own health: a pet holding enemies off its
            /// owner (a Voidwalker, a hunter's pet). Warlocks lost two packs in three with their pets doing a sixth
            /// of their damage and none of the holding (2026-10-02, stage5 at 60M).
            float PetTank = 0.5f;
            float PetTankMax = 1.5f;            // ... at most this an episode
            float MeleeRange = 3.5f;            // the range the approach shaping aims for, melee specs
            float RangedRange = 25.0f;          // ... ranged specs
        } Duel;

        /// How hard the creature duel's opponents are, per class/role. Tier t below EliteTier is a normal creature
        /// t x LevelsPerTier levels above the seat; from EliteTier on an elite, (t - EliteTier) x LevelsPerTier levels
        /// above. A class/role moves up a tier when it wins (kills without dying) RaiseAbove of Window fights at its
        /// tier, and down when it wins fewer than LowerBelow.
        struct DifficultyTuning
        {
            uint32 MaxTier = 6;
            uint32 EliteTier = 4;               // > MaxTier: no elites
            uint32 LevelsPerTier = 1;
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
            uint32 HazardChance = 30;
            /// The outcome terms scale with the tier: a win (kill, clear, health kept) is multiplied by
            /// 1 + TierScale x tier, a loss (death, timeout, overtime) divided by it. A tier-0 fight is unchanged;
            /// at tier 6 and 0.25 a kill pays 2.5x and a death costs 0.4x. Evaluations spread their seeds over
            /// every tier while training climbs per class, so with flat terms the score fell as the ladder rose
            /// -- every rung-6 loss cost as much as a rung-0 one -- and convergence read the fall as done. Scaled,
            /// the break-even win rate falls with the tier, so a hard fight is worth attempting, and the score is
            /// comparable across rungs. Fixed-bonus opponents (evade, hide, stealth) are not a ladder and stay flat.
            float TierScale = 0.25f;
        } Difficulty;

        /// Real instances (InstanceEncounter): where the raid stands and what a lost boss fight is worth.
        struct InstanceTuning
        {
            uint32 EngageYards = 35;            // how far back up the path from the boss the seats start
            uint32 TrashRadius = 60;            // creatures this close to the boss that are not its adds are cleared
            /// The rung's tier scale is capped here: a ladder of twenty bosses at 0.25 a tier would pay a top kill
            /// 5.75x, where the pool ladders stop at 2.5x. Kill, HealthKept and BossProgress are multiplied by the
            /// capped scale, Death and Timeout divided by it.
            uint32 MaxTierScale = 6;
            /// Paid on a wipe, an evade or the clock for the share of the boss's health the fight took off it, so a
            /// forty-seat fight has a gradient before its first kill: at 5, a wipe at 40% pays 3 (x the tier scale).
            /// It was not paid on the clock, so a raid that could not win learned that doing nothing cost the same
            /// as trying (stage13_raids, 2026-09-29: 0 kills, output falling, idle rising).
            float BossProgress = 5.0f;
            float Timeout = 10.0f;              // the clock, scaled by what is left of the boss (Duel.TimeoutFloor)
            /// Per second the boss has not been engaged once StallGraceMs of the episode are gone, as the single
            /// pack's Pulls.Stall.
            float Stall = 0.08f;
            uint32 StallGraceMs = 15000;
            /// Whole wings (InstanceLadder::Wing): each trash creature killed, each waypoint of the route reached, the
            /// wing's boss, each seat's death and each wipe (WingWipes of them end the episode; below that a wipe
            /// stands the party up at the door). One since 2026-09-30: a wipe ends the run, and the next run starts
            /// in a fresh instance with the whole dungeon reset -- never a second attempt on a half-cleared one. The kill and waypoint terms scale with the rung, the costs are divided by it.
            /// Raised 2026-09-30 so a wing pays for itself: over a 1,200 s run the per-press costs (jitter, repeat,
            /// effort, the combat clock) came to about -75, against 7.5 for 25 trash kills and 10 for a last boss
            /// no party reached. A dungeon takes long to learn; every step of it has to be worth taking.
            float WingTrashKill = 1.0f;
            float WingWaypoint = 0.5f;
            float WingBoss = 25.0f;
            float WingMidBoss = 8.0f;           // each dungeon boss killed on the way to the last
            /// The whole route walked, paid as it is walked (potential on the route still ahead). The waypoints are
            /// every WingWaypointYards now, which pay WingWaypoint each on top.
            /// 60 since 2026-09-30: at 20 the parties learned to clear what was near and stop short of the next pull.
            float WingProgress = 60.0f;
            float WingDeath = 3.0f;
            float WingWipe = 5.0f;
            uint32 WingWipes = 1;
            /// Per second once WingStallGraceMs pass with no kill, no step along the route and nothing fighting the
            /// party; and the clock's cost for the share of the route left (over the tier scale). Standing at the
            /// door has to cost more than fighting through the dungeon badly.
            float WingStall = 0.1f;
            uint32 WingStallGraceMs = 60000;
            /// The stall charge on every seat but the tank, as a share of the tank's: the tank decides when to move on.
            float WingStallOthers = 0.2f;
            /// Paid to the tank for each fight started with every living seat at WingReadyShare of its health and
            /// mana: pulling when the party is ready, not standing in front of the pack.
            float WingEngage = 1.0f;
            float WingReadyShare = 0.8f;
            float WingTimeout = 30.0f;
            uint32 WingWaypointYards = 30;      // the route's points are this far apart along the door-to-boss path
            /// The support ladder (StageScenario::WING_RUNGS): each rung fixes the dungeon script's share of seats,
            /// the level lift, the wipes to spare and the hint weight. WingProbe of training runs are probes -- no
            /// script, no hints, no instruction, at the rung's level and wipes -- and only they measure the policy:
            /// once WingRungRuns probes at a rung have made, on average, WingRungTarget of the dungeon (the share of its
            /// creatures killed, 1 for a clear), the ladder steps down; if the probes on a rung fall below
            /// WingRungFallback of what they made when it was stepped onto, it steps back up. Nothing on a clock. In a
            /// cluster the host's ladder decides for every machine, from all their runs.
            float WingProbe = 0.2f;
            uint32 WingRungRuns = 40;
            float WingRungTarget = 0.6f;
            float WingRungFallback = 0.5f;
            uint32 WingRungStart = 0;           // the rung a run starts on (a resumed run names the one it reached)
            /// The dungeon script's seats and hints on the ladder's rungs (WingRung::Script, ::Hint): a support, off by
            /// default, switched on when a rung has not stepped for a long stretch. Off, the rungs lift the level and
            /// spare wipes only, and every run learns from its own rewards.
            uint32 WingSupport = 0;
            uint32 WingSupplies = 60;           // food and drink each seat brings into a whole dungeon
            /// The instructed healer protects whoever is below this health share.
            float WingInstructHeal = 70.0f;
            /// Log a line for each wipe: where, what was fighting the party, and who died in what order.
            uint32 WingTrace = 1;
            /// Per second, for each hostile creature on the party past WingCrowdFree (a pack): the pull that ran into
            /// the next one. The Deadmines' parties had a median of eight on them when they wiped (2026-10-01).
            float WingCrowd = 0.15f;
            uint32 WingCrowdFree = 4;
            /// 1: the route visits every pack in the instance, side bosses and all, before the last boss.
            uint32 WingFullClear = 1;
            /// A dead seat nobody has raised this long after the fight ends rises at the door and walks back.
            uint32 WingRiseMs = 30000;
            /// 1: a closed door opens by itself when a seat reaches it out of a fight (before the use action existed).
            uint32 WingAutoDoors = 0;
            /// Per second a seat other than the tank is further than WingStrayYards from it (both alive): stay with the
            /// leader.
            float WingStray = 0.02f;
            float WingStrayYards = 25.0f;
            /// The pull drill (ArenaDefinition::PullDrill): one pack of the dungeon a run, the party started
            /// PullStartYards back along the route from it with the packs before it cleared. A clean pull -- the pack
            /// dead and nothing else in the fight -- pays PullClean, a second pack joining ends the run at PullExtra,
            /// and the clock running out with the pack alive costs PullTimeout: the tank in full, every other seat
            /// PullOthers of it. Standing about costs WingStall after PullGraceMs. The seats are at most PullLift
            /// levels above the dungeon's range, so the creatures' aggro radius is near the real one. The ladder
            /// opens packs by how far the nearest other pack stands (30, 22, 14 yd, then any) once PullRungRuns
            /// drills on the newest rung have pulled clean PullRungTarget of the time; each machine climbs its own.
            float PullClean = 5.0f;
            float PullExtra = 5.0f;
            float PullTimeout = 2.0f;
            float PullOthers = 0.5f;
            uint32 PullGraceMs = 20000;
            uint32 PullLift = 2;
            float PullStartYards = 35.0f;
            uint32 PullRungStart = 0;
            uint32 PullRungRuns = 100;
            float PullRungTarget = 0.7f;
        } Instance;

        /// Life outside the fight (the quest, gather and town stages): what the world around the seat is made of,
        /// and what it is paid for. The outcome terms scale with the band's tier (Difficulty.TierScale).
        struct LifeTuning
        {
            float StepCost = 0.0002f;           // per decision, as the travel stages charge
            float Progress = 2.0f;              // potential shaping on the distance to the waypoint, once per approach
            float Wasted = 0.1f;                // a press that did nothing (an interact with nothing in reach)
            float Death = 5.0f;                 // divided by the band's tier scale
            float QuestAccepted = 1.0f;
            /// Per objective of the quest, spread over its count (a kill of five pays a fifth), times the tier scale:
            /// a quest of two objectives pays twice what one of one pays. It was per quest, so an objective unit of a
            /// three-objective quest of ten paid 0.1 -- less than the fight for it cost.
            float QuestCredit = 3.0f;
            float QuestTurnIn = 10.0f;          // times the tier scale
            float QuestTimeout = 3.0f;          // the clock without a turn-in, less what was done, over the tier scale
            /// Per decision a complete quest is not handed in (45% of completed quests never were): the turn-in is
            /// the point, and walking off with a finished quest costs. Per 50 ms of tuning, as every per-decision
            /// term is (DecisionScale): a whole 600 s episode holding one costs 3.6, well under the turn-in's 10.
            /// It was 0.002, which at 250 ms decisions charged 24 an episode -- a deliver quest is complete the
            /// moment it is taken, and the trial's quest arena lost 10 an episode to it (2026-09-30).
            float CompleteHeld = 0.0003f;
            /// Training only: a quest item's source that dies without dropping it has its loot rolled again, up to
            /// this many times, until it does -- the kill is what the seat must learn to pay for, not the drop
            /// chance. 0 turns it off; evaluation never rolls again.
            uint32 DropRerolls = 50;
            /// Quest credit a seat takes in a place another group holds (WorldCoordinator), per share of the quest:
            /// the price of poaching. Small: the zone is shared, and a place held by nobody is fair. Against the
            /// credit itself (QuestCredit 3.0 per share, times a tier scale of 1 or more) it takes back a twelfth
            /// at most, so credit in a held place still pays -- going elsewhere is better when elsewhere will do,
            /// but staying out of a place with anyone in it, or out of a fight shared with them, is never the
            /// cheapest policy. A claim lapses ClaimHoldMs after its group stops working the place, so only a
            /// place someone is actually working is held. It was 0.5 (a sixth) until the plan's review.
            float Poach = 0.25f;
            /// How long a group holds a place it works (ms), and how near counts as working it (yards).
            uint32 ClaimHoldMs = 30000;
            float ClaimRadius = 25.0f;
            float GatherNode = 2.0f;            // per node gathered, times the tier scale
            float GatherSkillUp = 0.5f;         // per skill point gained
            float TownSold = 2.0f;              // for the starting junk's whole vendor value, pro rata
            float TownRepaired = 2.0f;
            float TownStocked = 2.0f;
            float TownEquipped = 3.0f;          // per upgrade put on
            float TownDone = 5.0f;              // sold, repaired, stocked and dressed before the clock
            float SenseRange = 100.0f;          // yards the seat's world features reach
            float ObjectiveRadius = 60.0f;      // the world's creatures this close to a quest objective's place come along
            uint32 ObjectiveSpawns = 24;        // ... up to this many per place (and around a gather ground)
            float NodeRadius = 150.0f;          // the nodes this close to the gather ground are the field
            uint32 NodeSpawns = 24;
            float TownRadius = 80.0f;           // the traders this close to the inn are the town
            uint32 TownCopperPerLevelSquared = 25;  // the seat's purse: level squared times this (level 20: 1 gold)
        } Life;

        /// Cast-time spells, from the duel stage on.
        struct CastingTuning
        {
            float TimeWasted = 0.03f;           // per second spent on a cast that did not finish
            /// Per second of cast time of a cast that finished, in combat (not out of it, so casting long spells at
            /// nothing earns nothing). It pays a long useless cast as readily as the right one, so keep it small next
            /// to what a cast does, which damage, healing and the kill already pay for.
            float TimeCompleted = 0.03f;
            /// Per cast the bot cut short itself, whatever it had spent on it. TimeWasted is proportional to the
            /// seconds lost, so a cast stopped on the decision after it began costs almost nothing: under a
            /// deterministic policy that leaves start-cast / stop-cast a free loop to sit in for a whole episode
            /// (stage1_duel: a quarter of the warlock evaluation episodes, up to 299 cancels in one). A flat charge
            /// prices the loop -- hundreds of them outweigh anything an episode can pay -- while leaving the
            /// handful of deliberate stops a fight actually wants cheap next to the kill, so when to cut a cast
            /// short stays the policy's call.
            float Cancel = 0.05f;
        } Casting;

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
            /// brought back up, a place reached, a corpse, node or objective done. Recover and Rest pay by the share
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
            float WorldValue = 0.2f;
            /// The secondary goal (a second slot beside the primary: Fight A and hold B): paid this share of what the
            /// primary would be for reaching it and for closing on it, and charged Secondary every decision it is held,
            /// so covering everything is not free.
            float SecondaryShare = 0.5f;
            float Secondary = 0.002f;
        } Goals;

        /// The director's orders (TeamOrder), in every arena that has one.
        struct OrderTuning
        {
            /// Per decision a seat spends fighting the enemy its director called, while one is called and alive.
            ///
            /// Compliance shaping, and the plan is right that an order should end up being followed because
            /// following it wins fights rather than because it pays. But an advisory channel that correlates
            /// with nothing cannot bootstrap into that: stage19_duo_led ran 30M steps with the order paying
            /// nothing, and order_focus_kept sat at chance (0.45 -> 0.42, no trend) while the director's own
            /// entropy fell 0.17 nats. Seats ignored the call, so the director's actions changed nothing in the
            /// world, so its advantage was noise and it never left exploration. This is the same job
            /// Goals.Match does for the goal head -- keep the channel from collapsing into nothing -- and it
            /// should be annealed towards zero once order_focus_kept holds up without it.
            ///
            /// Paid per decision rather than once on arrival, unlike Goals.Match: holding a called target is
            /// the behaviour wanted, not a place to reach, and paying once per call would pay a side afresh
            /// every time its director changed its mind.
            ///
            /// Small because a per-decision term accumulates over a whole fight. At 0.01 it earned 5.01 an
            /// episode, 23.7% of the stage's gross reward, level with the kill (5.23) and the death (-5.27)
            /// and 70 times goal_match (0.07): a seat paid more for staying on the called target than for
            /// winning would tunnel on it past every reason to switch. This is the mistake Goals.Match's own
            /// comment records -- paying to sit in a state made standing at range the stage's second largest
            /// earner. At 0.001 full compliance is worth about 0.5 an episode, a tenth of the kill: enough to
            /// break the tie between fighting whoever and fighting the one called, and never enough to outbid
            /// the fight itself.
            float Focus = 0.001f;
            /// Paid once to a seat that arrives where its director sent it (TeamRally::Point), never per
            /// decision, and not again for PlaceCooldownMs. A transition with a cooldown cannot be farmed by
            /// stepping back and forth across the edge of the radius, which is exactly what a per-decision
            /// payment would buy -- the per-decision version of the focus nudge reached 23.7% of gross before
            /// it was cut.
            float PlaceMatch = 0.02f;
            float PlaceRadius = 8.0f;           // yards: close enough to count as arrived
            uint32 PlaceCooldownMs = 10000;
        } Order;

        /// Getting away: the stages about breaking off a fight that cannot be won.
        struct EvadeTuning
        {
            /// Paid once for going from seen to unseen, and not again for BreakCooldownMs. Never per second
            /// unseen: the best policy for paid seconds is to run to the far corner at the start and stand
            /// there, which is not evasion, and the reward audit would only say so after the run was spent.
            float BrokeContact = 0.3f;
            uint32 BreakCooldownMs = 5000;
            /// Unbroken seconds out of sight that count as having got away, for the `escaped` metric.
            uint32 EscapeMs = 8000;
        } Evade;

        /// Stalking: closing on someone while stealthed, and staying there. The stealth stage's own lesson,
        /// and the one thing here that is paid per decision rather than on a transition.
        struct StealthTuning
        {
            /// Paid each decision the seat is stealthed, unseen, and within StalkYards of its opponent, up to
            /// StalkMax an episode. Per-decision shaping is what the order nudge had to be cut for, so this
            /// one is capped outright: what it is worth is fixed no matter how long the episode runs, and the
            /// opener it sets up (Combat.StealthOpener) stays the larger prize.
            ///
            /// Time unseen is still never paid. The difference is the distance condition: staying stealthed
            /// inside StalkYards of something that is actively looking is the skill being taught, where
            /// staying unseen in the far corner of the map is the absence of one.
            float Stalk = 0.02f;
            float StalkYards = 10.0f;
            float StalkMax = 1.0f;
            /// Paid OpenerWindowMs after a stealth opener lands, per share of the opponent's health it had lost
            /// since (all of it, if the opener's burst killed it). The flat Duel.StealthOpener pays for landing one
            /// at all, the same for a wasted Ambush as for a Cheap Shot into a kill, so in the first run of the
            /// stealth drill the openers were already landing in 80% of fights and the score did not move for
            /// 30M steps (2026-09-28): nothing paid for an approach good enough to decide the fight.
            float OpenerDamage = 2.0f;
            uint32 OpenerWindowMs = 6000;
        } Stealth;

        /// What the director's own calls mean in yards.
        struct DirectorTuning
        {
            /// How far a place sits off its anchor, at each ring. The whole point of naming a ring rather
            /// than a distance is that these two numbers are all that changes between an arena and a
            /// continent: the thirteen place actions mean the same thing at any scale.
            float PlaceNearYards = 20.0f;
            float PlaceFarYards = 60.0f;
            /// How often the director's clock gives it a turn (decisions; 10 is 2.5 s). Events give it one at once:
            /// a member down or newly below a quarter of its health, a new enemy in the fight, the focus dead.
            uint32 ClockDecisions = 10;
            /// Below this share of its health a member counts as badly hurt, for the event.
            float LowHealth = 0.25f;
            /// A member order stands this long (decisions; 8 is 2 s) before the same source can replace it for
            /// free: one replaced sooner costs the director OrderChurn, on top of OrderChange for any live order it
            /// replaces. Not a rule -- nothing stops the call -- so the director learns to let orders stand; the
            /// raid director gave each member about 9,500 orders an episode at the start of stage12.
            uint32 OrderHoldDecisions = 8;
            float OrderChange = 0.01f;
            float OrderChurn = 0.03f;
        } Director;

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
            /// The same charge where the episode already prices mana honestly: a gauntlet pays readiness for what a
            /// seat brings to the next pull (SoloGauntletReadiness, OwnerReadiness), so mana spent healing already
            /// costs it there, and charging again would price the same mana twice. HealingMana is a stand-in for an
            /// opportunity cost, needed only where there is no later fight to have it -- a duel ends at the kill and
            /// leftover mana is worth nothing, which is where efficiency has to be taught. 0 leaves the gauntlet's
            /// own accounting to do the work.
            float HealingManaWithReadiness = 0.0f;
            /// Gauntlets: engaging a pull pays this times the share of the layout's buff groups up on the seat (and on
            /// the owner, averaged, with one), next to readiness.
            float BuffCoverage = 0.3f;
            /// A class that keeps a pet (PetBlock::HasPet) with it out when a fight starts, paid once at the
            /// engagement. A pet is part of being ready, and the kill alone did not teach it: the warlock summoned
            /// in 7% of the episodes it did not start with one where the hunter summoned in 85% of its own.
            float PetReady = 0.3f;
        } Support;

        /// How often a seat may press the same button, as a player would. Each decision is 100 ms apart, and a
        /// policy free to act on every one of them re-issues orders nobody would: stage1_duel's warlocks sent their
        /// pet in 125 times an episode and started and stopped a cast 26 times while standing out of the fight. A
        /// paced action is masked until it may be pressed again, so the policy never sees the loop as an option.
        /// Spells keep their own global cooldown and cooldowns as well. 0 turns a pace off.
        struct ActionTuning
        {
            uint32 RepeatMs = 1000;             // the same action again: spells, orders, consumables, targeting
            uint32 MoveRepeatMs = 300;          // the same movement order again (steering stays responsive)
            uint32 StopCastMinMs = 500;         // a cast the bot is in cannot be stopped before it ran this long
            uint32 RecastAfterStopMs = 2000;    // a spell the bot stopped itself cannot be started again for this long
            /// A stance, form, presence, aspect, aura, seal, armor or pet stance holds this long before another change
            /// of its kind: warrior tanks changed stance 22 times a fight, hunters their aspect 12.
            uint32 ModeLockMs = 5000;
            /// Pressing the same action over and over. Pacing caps how often an action can be pressed, not how many
            /// times in a row: stage1_duel's warlocks gave their pet 93 orders an episode, a second apart, and the
            /// pet dealt 1% of their damage. Each press of an action counts the presses of that same action within
            /// the last RepeatWindowMs; past the free ones, each costs Repeat. How often the seat acts overall is not
            /// charged, only the same button again, and movement orders never are: steering is always free.
            float Repeat = 0.03f;
            uint32 RepeatWindowMs = 10000;
            uint32 RepeatFree = 3;              // presses of one action within the window that cost nothing
            /// Steering that does not commit, per quarter turn a turn, a pitch or a bearing takes back of the one
            /// before it, weighed by how recent that was (e^(-dt / Options.JitterDecayMs)); a facing mode taken back
            /// and a start moments after a stop at one each (MovePrice, movement-smooth C). Nothing in the rewards
            /// cared how a seat got where it was going, so a wobble that cost nothing was learned as harmless: in the
            /// first full run's final evaluations 63-70% of the ground stages' turns were undone within three
            /// decisions, and in flight the feet changed bearing every quarter second (2026-09-28). Small, like Repeat:
            /// a steady course is the habit it teaches, and a real reason to turn back -- a target that moved -- still
            /// outweighs it.
            /// Raised from 0.02 after the next-run trial, where bearing flips ran twice the last run's and did not
            /// fall over 20M steps (2026-09-30).
            float Jitter = 0.05f;
            /// **Presses with intent** (StageScenario::JudgePress). Every spell and movement press is judged against
            /// the goal the seat holds: it serves it (damage on the focus under Fight, a heal on someone else under
            /// Protect, a step that closes on the wanted range under Position), is neutral (an interrupt, a
            /// defensive when hurt, anything with no goal to judge by), or is aimless -- a press that works against
            /// the goal the seat itself chose. Aimless presses cost Aimless. In-game testing of the second run's
            /// models found ~180 actions a minute with 18-21% of decisions serving the chosen goal (2026-09-28):
            /// abilities pressed without intent. The goal is the reason for a press; this is what makes it one.
            float Aimless = 0.02f;
            /// Aimless by its cause (StageState's AimlessCause), each its own price so the trial can raise one
            /// without the rest. The ones in-game testing of the four-phase models saw most start higher: a
            /// companion drinking at full mana, laying traps with nothing near, dancing between aspects, and
            /// moving about a fight it could stand and shoot in (2026-09-29); switching targets and sending the
            /// pet at enemies the goal does not name (6.5 times over-represented in the aimless-heavy episodes).
            float AimlessOffFocus = 0.02f;
            float AimlessAoeMissed = 0.02f;
            float AimlessInRangeCast = 0.02f;
            float AimlessUnprovokedHarm = 0.02f;
            float AimlessHelpOffGoal = 0.02f;
            float AimlessStepAway = 0.02f;
            float AimlessTargetSwitch = 0.04f;
            float AimlessPetOffGoal = 0.04f;
            float AimlessConsumeNotNeeded = 0.03f;
            float AimlessTrapNoEnemy = 0.03f;
            float AimlessModeFlip = 0.03f;
            float AimlessModeReverse = 0.06f;
            float AimlessNeedlessMove = 0.02f;
            /// A taunt from a healer or damage dealer beside a living tank (holy paladins taunted four times a fight
            /// from the healer's seat, 2026-10-03). At 0.04 Hand of Reckoning from the healer's seat rose through
            /// stage6, 3.3 to 4.1 a fight between 20M and 62M steps, while Righteous Fury at the same price halved.
            float AimlessTauntOffRole = 0.15f;
            /// A tank's stance, form, aura or presence from a seat that is not the tank, beside a living one (holy
            /// paladins took up Righteous Fury three and a half times a fight from the healer's seat, 2026-10-03).
            float AimlessTankModeOffRole = 0.04f;
            /// A press that failed for something the seat controls: facing away (or not behind), out of range or too
            /// close, out of sight, a cast time pressed on the move, short of power. Offered rather than masked, so
            /// the seat learns to put each right before it presses (2026-10-04).
            float AimlessCastFailed = 0.02f;
            /// A sight block press the world refused (dungeon-curriculum I1, EntityActions::Refusal): an entity gone or
            /// out of reach, a thing the press does not take, no key item, a cast refused. Offered, never masked: a
            /// press on a remembered entity is the world's to judge.
            float AimlessActRefused = 0.02f;
            /// Every aspect, stance, form or presence changed, justified or not: a change has to be worth something.
            float ModeSwitch = 0.01f;
            /// Every food or drink consumed: a supply spent at full health is gone when it is needed.
            float SupplySpent = 0.02f;
            /// Resource at or above which eating (health) or drinking (mana) is ConsumeNotNeeded.
            float ConsumeFullPct = 85.0f;
            /// A small price on every press but the no-op, a tenth of an aimless one: when nothing needs doing,
            /// doing nothing wins. A held bearing keeps walking and a cast keeps casting without another press. Raised
            /// from 0.002 after the next-run trial (2026-09-30): with the per-cause prices, combat APM still ended at
            /// 99 in the gauntlet and 89 in the pack, against a band of 30-70.
            float Effort = 0.004f;
            /// In a fight, per second spent moving while already at the range the spec wants, with nothing on the
            /// ground to step out of: the shuffle that reads as a bot. Moving to reach range, to dodge, or out of a
            /// fight is untouched.
            float Fidget = 0.01f;
            /// How long Fidget's and NeedlessMove's conditions must hold before they are charged (ms): a seat that
            /// runs into the band it wants and stops within it is not fidgeting, and a range that flickers at its
            /// edge is not charged on every flicker (movement-smooth C).
            uint32 SettleGraceMs = 500;
            /// How much the gap to the wanted range has to change for a step to count as closing or opening it.
            float IntentSlackYards = 0.5f;
        } Actions;

        /// Packs and the gauntlet's pull after pull.
        struct PullTuning
        {
            int32 LinkedChance = 70;            // percent of pulls whose members aggro together
            int32 EliteChance = 15;             // gauntlet: a single elite instead of a pack
            int32 HigherLevelChance = 25;       // gauntlet: a pack 1-3 levels above (1 below level 20, 2 below 30)
            int32 PartyEliteChance = 50;        // party: per pack member
            uint32 NextPullMinMs = 8000;        // gauntlet: the break between pulls
            uint32 NextPullMaxMs = 20000;
            uint32 OwnerEngageMinMs = 1500;     // with an owner: when it walks over to a new pull
            uint32 OwnerEngageMaxMs = 5000;
            uint32 PartyOwnerEngageMinMs = 4000;    // ... in a party, after the tank has had time to pull
            uint32 PartyOwnerEngageMaxMs = 7000;
            uint32 OwnerPullsMinMs = 500;       // ... when the owner starts the pull itself
            uint32 OwnerPullsMaxMs = 1500;
            int32 OwnerPullsChance = 30;        // percent of pulls a damage dealer or healer owner starts (tanks: all)
            float RecoverFraction = 0.5f;       // owner stages: health and mana the dead stand up with after a pull
            // Rewards.
            float DamageDealt = 2.0f;           // fraction of the pull's total health
            float DamageTaken = 1.0f;           // pack: fraction of the bot's health
            float GauntletDamageTaken = 1.5f;   // gauntlet on: surviving many pulls matters more than any one
            float Approach = 0.5f;
            float StealthOpener = 0.5f;
            float StealthUtility = 0.05f;
            float Interrupt = 0.3f;
            /// An interrupt is paid by what it prevented, as a multiple of Interrupt: a heal undoes damage already
            /// dealt, an area spell would have hit everyone, a long cast was a large part of the caster's output.
            /// Never below 1 -- the flat term is how a class finds interrupting at all, and paying only for heals
            /// risks the behaviour never appearing to be shaped (stage 2: the classes that interrupt found it
            /// through the flat term).
            float InterruptHeal = 3.0f;
            float InterruptArea = 2.0f;
            float InterruptLong = 1.5f;
            float Kill = 0.5f;
            float StepCost = 0.0002f;           // per decision
            /// Owner arenas (stages 4, 5, 8), win-first as the solo gauntlet: each pull cleared pays Clear plus
            /// FastPull times 1 - its time since engaged / 60 s, both x OwnerClearScale, and HealthKept times the
            /// seat's own health kept through it; the seat's death costs GauntletDeath and the owner's Owner.Death. At
            /// 2 + 2 (doubled), 2 and 5 against an owner's death of 6, a pull cleared was worth more than the owner's
            /// life, and the seat's own health as much as guarding it.
            float Clear = 2.5f;
            float FastPull = 0.5f;
            float HealthKept = 0.5f;
            float GauntletDeath = 10.0f;
            /// A single pack is won or lost, as the duel is: the clear outweighs finishing it untouched, and dying or
            /// running out of time costs as much as the clear pays. Clear and HealthKept had the pack worth 2 + 2, so
            /// keeping health paid as much as winning, and a death cost only 3.
            float PackClear = 10.0f;
            float FastClear = 1.0f;             // pack: times the episode fraction left after engaging
            float PackHealthKept = 0.5f;        // pack: times the health kept through the pack
            float PackDeath = 10.0f;
            /// Pack: the top rung of the single pack's ladder (PullsEncounter's PACK_RUNGS, 0-5), climbed per
            /// class/role with the Difficulty.* rates. 0 keeps every pack on the first rung.
            uint32 MaxTier = 5;
            float Timeout = 10.0f;              // pack: the clock ran out with the pack and the seat both alive
            float TimeoutFloor = 0.5f;          // pack: the share of it charged whatever the pull's progress
            /// A timeout charged only at the end is 150 s away when the kiting starts: the discount leaves about a
            /// fifth of it, against a whole death now, so running out the clock looked safe. A fight engaged longer
            /// than OvertimeGraceMs is charged as it drags on, and a death in overtime is charged the overtime left,
            /// so dying never ends it more cheaply than the timeout would.
            float Overtime = 0.1f;              // pack: per second of a fight past OvertimeGraceMs since it was engaged
            uint32 OvertimeGraceMs = 60000;
            /// Pack: crowd control priced as the damage it prevents, in the seat's own maximum healths, so it is in
            /// the currency DamageTaken is already charged in and the two weights are comparable. The divisor is
            /// *current* health, floored at ControlHealthFloor of the maximum: preventing a hit matters more the less
            /// health there is to lose, which is what makes control a survival tool rather than a damage discount.
            /// Priced at half DamageTaken: the damage a held enemy would have dealt is estimated from what it dealt
            /// while loose, not observed, and the health floor can multiply it fivefold, so control is paid less than
            /// the damage it is credited with preventing. Measured at 0 through 2026-09-18 (stage2_pack at 30M:
            /// control_prevented 0.03 healths a fight, reward_control 0.00 -- nothing was controlled because nothing
            /// paid for it).
            float SinglePackControl = 0.5f;
            float SinglePackControlMax = 1.0f;  // ... at most this per pull, a guard rather than a shaping knob
            float ControlHealthFloor = 0.2f;
            float ControlFallbackDps = 0.02f;   // maximum healths per second, for an enemy that never got to act
            uint32 ControlRateMinMs = 3000;     // free-to-act time before an enemy's own measured rate is trusted
            /// Pack: control time extends the overtime grace, up to this much, so holding an add is not charged as
            /// dragging the fight out -- with the grace alone, the overtime charge took back what the control paid.
            /// 0 leaves the grace alone. Bounded on purpose: Overtime exists to stop kiting the clock, and an
            /// unbounded pause would hand that back.
            uint32 ControlGraceMaxMs = 15000;
            float Stall = 0.08f;                // pack: per second not engaged once StallGraceMs are gone
            uint32 StallGraceMs = 15000;
            uint32 PreparationRefundMaxMs = 15000;  // pack: as the duel's
            float Spacing = 0.1f;               // pack: per second a ranged spec is hit in melee reach (0.03 left
                                                // casters in melee 70-80% of pack fights, 2026-09-28; 0.1 with the
                                                // duel's, 2026-10-02)
            /// A camp (PullSchedule::Camp, the pull drill): per second, for each pack fighting beyond the first; paid
            /// for each pack killed with no other pack in its fight (times the rung's scale); and the grace between
            /// packs, from the last fight, before standing about is charged as Stall (not while eating or drinking).
            float CampExtraPack = 0.15f;
            float CampCleanPack = 2.0f;
            uint32 CampRestMs = 25000;
            /// A gauntlet alone (no owner) is won by lasting: pull after pull until the episode ends, and a death ends
            /// it with every pull left unfought. Clear 2 + FastPull 2 and HealthKept 2 had each pull worth up to 6
            /// against a death at 5, so a seat could trade its life for a fast pull. As the single pack: the clear
            /// outweighs finishing it fast or untouched, and a death costs two clears besides the pulls it forfeits.
            /// Stall and Spacing apply to its pulls as to a single pack's (Stall from each pull's spawn). Owner stages
            /// keep Clear, FastPull, HealthKept and GauntletDeath.
            float SoloGauntletClear = 5.0f;
            float SoloGauntletFastPull = 1.0f;  // times 1 - time since the pull engaged / 60 s
            float SoloGauntletHealthKept = 0.5f;
            float SoloGauntletDeath = 10.0f;
            /// Paid when a pull is engaged, times the seat's health fraction the decision before, or the lower of its
            /// health and mana fractions if it uses mana: entering a fight ready is what resting between pulls is for.
            float SoloGauntletReadiness = 0.5f;
            /// Lasting to the end wins only with this many pulls cleared: a gauntlet is endured by fighting it, not
            /// by staying away from it.
            uint32 SoloGauntletWinPulls = 5;
            uint32 GauntletSupplies = 7;        // solo gauntlet: food and drink stocked, each
            /// Solo gauntlet: per second per pack member kept out of the fight once the pull is engaged -- stunned,
            /// incapacitated, asleep, polymorphed, feared, or rooted out of melee reach and not casting -- other than
            /// the seat's target, while another member is alive. It stops when the control breaks, so controlling an
            /// add and hitting it pays nothing. At most SoloGauntletControlMax per pull: a fight isn't worth dragging
            /// out for it.
            float SoloGauntletControl = 0.02f;
            float SoloGauntletControlMax = 1.5f;
            /// Solo gauntlet pacing. A pull nobody has engaged comes to the seat ArriveMinMs-ArriveMaxMs after it
            /// spawns, so resting has a clock; each pull cleared brings the next one sooner (ArriveShrinkMs, down to
            /// ArriveFloorMs) and shortens the break before it (NextPullShrinkMs, down to NextPullFloorMs).
            uint32 ArriveMinMs = 20000;
            uint32 ArriveMaxMs = 40000;
            uint32 ArriveShrinkMs = 1500;
            uint32 ArriveFloorMs = 10000;
            uint32 NextPullShrinkMs = 1000;
            uint32 NextPullFloorMs = 4000;
            /// Gauntlets (stages 3-5, 8): what the per-hit terms -- damage dealt, damage taken, kills, approach --
            /// are multiplied by. A plan pays at the end of a pull or an episode (the clear, surviving, readiness,
            /// control), and dense terms paid every decision drown those out: a seat that opens on the nearest enemy
            /// and never stops earns most of what a careful one does, minutes sooner. Below 1 the outcome is what the
            /// stage is about; 1 leaves the single pack's balance alone.
            float GauntletDenseScale = 0.5f;
            float OwnerClearScale = 2.0f;       // owner stages: kills and clears count this many times
            /// Owner arenas keep what the solo gauntlet teaches: readiness paid when a pull is engaged (the lower of
            /// health and mana), crowd control that keeps an add out of the fight (per enemy-second, capped per pull),
            /// and a win: lasting to the end with the owner never dead, no wipe and OwnerWinPulls pulls cleared,
            /// counted as the kill so clean_kill is the gauntlet won beside the owner.
            float OwnerReadiness = 1.0f;        // was 0.5: parties still pulled ~3 times an episode with someone low
            float OwnerControl = 0.02f;
            float OwnerControlMax = 1.5f;
            uint32 OwnerWinPulls = 5;
        } Pulls;

        /// The scripted owner of the companion and party stages.
        struct OwnerTuning
        {
            int32 LevelSpread = 2;              // its level: the bot's plus or minus this
            int32 TankChance = 25;              // percent tanks, healers, the rest damage dealers
            int32 HealerChance = 25;
            /// In a cast-owner arena (ArenaDefinition::OwnerCast), the percent of training episodes whose owner
            /// is still the script rather than the frozen checkpoint: the script wanders and engages on a
            /// timer, which is the owner the follow lesson was built on, and a frozen solo policy may just stand
            /// between pulls. Evaluations always script it.
            int32 CastScriptedShare = 30;
            // Rewards added to the pulls'.
            float DamageTakenDps = 1.0f;        // damage dealers: the owner's damage taken, fraction of its health
            float DamageTakenProtector = 2.0f;  // tanks and healers exist to prevent it
            float TankOwnerDamageShare = 0.25f; // a tank owner is hit by design: its damage taken counts this much
            /// Any role: effective healing and protection on the owner, as a fraction of its health. 3, above
            /// Party.TeammateHealing's 2: the owner is the one whose death costs the most (Death 15 against
            /// TeammateDeath 3), and at equal pay the party stage's healers tripled their teammate healing while their
            /// owner healing fell back to its start (2026-09-28).
            float Healing = 3.0f;
            float TankDamageRefund = 0.5f;      // tanks: soften the pulls' damage taken
            float TankHold = 0.006f;            // tanks: per enemy on the tank, per decision (was a tenth of TankLose)
            float TankLose = 0.02f;             // tanks: per enemy on the owner, per decision
            float PulledThreat = 0.004f;        // damage dealers and healers beside a TANK owner: per enemy on
                                                // the bot, per decision; not charged beside any other owner
            float SoloFight = 0.01f;            // per decision in combat while the owner is not
            // Staying close: in-game testing of the four-phase models found companions trailing about 16 yards
            // where a player keeps 3-6 (2026-09-29). Near is now the band a player keeps, far starts where a
            // player would call it lost, and a moving owner charges every yard it is trailed by past the band.
            float FollowFar = 0.004f;           // per decision out of combat beyond FollowFarDistance
            float FollowNear = 0.002f;          // per decision out of combat within FollowNearDistance
            float FollowFarDistance = 15.0f;
            float FollowNearDistance = 6.0f;
            float FollowTrail = 0.001f;         // per decision and yard past FollowNearDistance while the owner moves
            float Death = 15.0f;                // per owner death, every seat: more than the seat's own (GauntletDeath)
        } Owner;

        /// Durative actions (SeatOption): how long each may run before the seat has to choose again. They end on
        /// their own conditions too, and any other action the policy takes cancels them.
        struct OptionTuning
        {
            uint32 RestMaxMs = 30000;           // eat and drink until health and mana are back
            uint32 HoldInterruptMs = 10000;     // interrupt the target as soon as it casts
            /// How fast a steering choice stops weighing on the one that undoes it (Actions.Jitter): its weight is
            /// e^(-dt / this). Replaces a window, JitterWindowMs (1500), that charged in full up to its edge and
            /// nothing past it, so a slow weave (a period of two seconds or more) was free. At 2500 a reversal 2 s on
            /// weighs 0.45, 3 s 0.30, and a deliberate correction 5 s on 0.14 (movement-smooth C).
            /// The window's history: about three decisions: long enough to catch a head twitching side to side, short
            /// enough that a seat that walked one way for a moment and then chose another is not charged for having
            /// changed its mind. Six decisions since the next-run trial (2026-09-30): at three, a seat that swung back
            /// a second later went uncharged, and bearing flips did not fall.
            uint32 JitterDecayMs = 2500;
        } Options;

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

        /// The rotation drill (Opposition::Dummy, DummyEncounter): output against targets that do not fight back.
        struct DummyTuning
        {
            float HealthScale = 20.0f;          // a dummy's health, times its level's own: it outlives the episode
            float HittingHealthScale = 4.0f;    // the one that hits back: a long fight, but one that can be won
            float Damage = 1.0f;                // per the dummy's own (unscaled) health dealt: a kill's worth of output
            float Kill = 2.0f;                  // the hitting dummy killed
            float Death = 3.0f;
            float Bleed = 0.012f;               // the bleeding drill: share of the seat's health lost a second, average
            float Hurt = 0.3f;                  // ... per second, per share of the seat's health missing
            float Resource = 0.5f;              // at the end, per share of the mana bar kept
            uint32 AddEveryMs = 15000;          // the moving drill: another dummy about this often
            uint32 MaxAdds = 2;
        } Dummy;

        /// The movement stages' markers (Opposition::Markers, MarkerEncounter): a place to stop on, then the next.
        ///
        /// The ladder is per class and build (DifficultyLadder, Difficulty.*), Rungs rungs from the first to the last,
        /// each of the three things it tightens moving linearly between its First and Last value: how far the marker
        /// is (DistanceMin up to Distance*), how far round from the seat's facing it may be (Bearing*, degrees either
        /// side: 180 is behind as well) and the radius the seat has to stop in (Radius*). A rung counts as won when
        /// every marker of the episode was reached before the clock.
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
            uint32 MarkersMin = 3;              // markers an episode (drawn per episode)
            uint32 MarkersMax = 8;
            uint32 Rungs = 8;                   // rungs on the ladder, the last of which is the stage's real task
            float DistanceMin = 5.0f;           // yards: the nearest a marker is, at every rung
            float DistanceFirst = 10.0f;        // the furthest, on the first rung ...
            float DistanceLast = 60.0f;         // ... and the last
            float BearingFirst = 20.0f;         // degrees either side of the facing a marker may be, first rung
            float BearingLast = 180.0f;
            float RadiusFirst = 4.0f;           // yards: the radius to stop in, first rung (user, 2026-10-05: 4 -> 0.5)
            float RadiusLast = 0.5f;
            /// The way to a marker on the navmesh may be at most this many times the straight line: M1's markers are
            /// in the open, where the straight line is the way.
            float MaxDetour = 1.1f;
            /// How near a stop has to be to count in stop_distance (a stop far from the marker is a pause, not a try).
            float StopNear = 10.0f;
            /// Arriving is on the marker's own floor too: the unit within this many yards of its height (a seat under
            /// a ledge, or a storey below, is not on it).
            float ArriveRise = 2.0f;
            /// The deepest single drop a marker's walking way may take, every course (TravelPlaceRules::RouteMaxDrop;
            /// a ledge's way round takes MarkerVertical.SafeDrop instead). Placement never asks for a near-fatal
            /// fall: at 20 yd a fall takes 0.018 x 20 - 0.2426 = 12% of maximum health (nothing under 13.48 yd).
            float RouteMaxDrop = 20.0f;
            /// The most a stage's narrow legs (above, below, upstairs, across water, a lakebed) may fall back to
            /// ordinary markers: a class cannot converge while its top-rung evaluation's fallback share is over it
            /// (the learner reads it from stage.json), and a spawn point over it across FallbackMinLegs legs is
            /// named once in the log and in `forge status`, to be removed from the data.
            float FallbackCeiling = 0.2f;
            uint32 FallbackMinLegs = 50;
        } Markers;

        /// The ground stage's markers (MarkerCourse::Ground, M2): broken ground with something in the way -- a face
        /// too steep, a fence line, a rock field, a wood -- so the straight line is often not the way. Arrive,
        /// StepCost, Death and Progress are Markers.*; Progress is shaped on the route (the route planner's distance,
        /// a training signal only), and there is no Facing term (the marker's bearing is not the way here).
        ///
        /// The ladder (Rungs rungs) moves the furthest distance from DistanceFirst to DistanceLast (the nearest is
        /// DistanceMin) and the detour -- the walking way over the straight line -- from DetourFirst to DetourLast:
        /// a marker's detour is at least the rung's and at most DetourSpan more (the floor let go after half the
        /// placement attempts, so ground without one still builds; the `detour` column says what was got). The stop
        /// radius is Radius at every rung (M1 has taught the stop).
        ///
        /// The costs, noise prices on the cost ladder: Stuck per second of a movement key held with the body getting
        /// nowhere for a second or more (the controller's stuck_seconds), Wall per second pressing into a wall
        /// (wall_seconds).
        struct MarkerGroundTuning
        {
            uint32 MarkersMin = 2;
            uint32 MarkersMax = 4;
            uint32 Rungs = 6;
            float DistanceMin = 20.0f;
            float DistanceFirst = 40.0f;
            float DistanceLast = 120.0f;
            float DetourFirst = 1.0f;
            float DetourLast = 1.6f;
            float DetourSpan = 0.25f;
            float Radius = 1.0f;
            float Stuck = 0.05f;                // per second
            float Wall = 0.03f;                 // per second, scaled by how blocked the seat was (WallSlide)
            /// The controller's wall seconds count every tick a step met a wall, a slide along it too, which is the
            /// right way round a corner's inside. So Wall is charged only for the part of the decision's ground not
            /// covered: nothing while the unit moved at least WallSlide of what its held keys ask, rising to the full
            /// price at no movement (MarkerEncounter::WallCharge).
            float WallSlide = 0.5f;
        } MarkerGround;

        /// The vertical stage's markers (MarkerCourse::Vertical, M3): up and down. By the arena's ground: above the
        /// seat from a cliff foot or a terrace (the way up a ramp, a stair or a jump), below it from a ledge top (the
        /// drop the shortcut, the way round the safe one; ArenaDefinition::Ledges), or on another floor of a building
        /// (ArenaDefinition::Indoors). No interactions and no closed doors: a marker whose way the controller cannot
        /// walk -- a closed door is a wall to it -- is never placed (TravelPlaceRules::ControllerReach).
        ///
        /// The ladder (Rungs rungs) moves the height window from [HeightMinFirst, HeightMaxFirst] to
        /// [HeightMinLast, HeightMaxLast] yards (above for a climb, the drop for a ledge, either way indoors) and the
        /// furthest distance from DistanceFirst to DistanceLast (the nearest DistanceMin). A ledge's way round takes
        /// no drop deeper than SafeDrop. FallDamage is charged per share of the seat's health a fall took (a Cost,
        /// always at full price): what a drop costs is the seat's to learn, and the deep ones kill (Death).
        struct MarkerVerticalTuning
        {
            uint32 MarkersMin = 2;
            uint32 MarkersMax = 4;
            uint32 Rungs = 6;
            float DistanceMin = 10.0f;
            float DistanceFirst = 30.0f;
            float DistanceLast = 80.0f;
            float HeightMinFirst = 1.0f;
            float HeightMaxFirst = 6.0f;
            float HeightMinLast = 15.0f;
            float HeightMaxLast = 45.0f;
            float Radius = 1.0f;
            float SafeDrop = 6.0f;
            float FallDamage = 2.0f;            // per share of maximum health a fall took
            /// Rooms: the share of legs whose marker is a storey or two up (by the stairs, UpstairsRise yards over
            /// the seat at most); the rest are down or on the seat's own floor. Stairs are the commonest vertical
            /// move a player makes, so they are asked for, not left to where a spawn lands.
            float RoomUpShare = 0.5f;
            float UpstairsRise = 12.0f;
        } MarkerVertical;

        /// The water stage's markers (MarkerCourse::Water, M4). By the arena's ground: a marker across water (Water:
        /// a crossing whose dry way round is the longer one, so swimming is a choice with a price either way), on a
        /// lakebed (Underwater: arriving is stopping there, swimming, within the radius and ArriveRise of it), or a
        /// chain of lakebeds (Underwater and Checkpoints: ChainMin to ChainMax of them, longer than a breath).
        ///
        /// The ladder (Rungs rungs) moves the furthest distance from DistanceFirst to DistanceLast (the nearest
        /// DistanceMin) and a lakebed's depth window from [DepthMinFirst, DepthMaxFirst] to [DepthMinLast,
        /// DepthMaxLast] yards. Drowning is charged per share of the seat's health the water took (a Cost at full
        /// price); a drowned seat's death is Markers.Death.
        struct MarkerWaterTuning
        {
            uint32 MarkersMin = 2;
            uint32 MarkersMax = 3;
            uint32 ChainMin = 4;
            uint32 ChainMax = 6;
            uint32 Rungs = 6;
            float DistanceMin = 15.0f;
            float DistanceFirst = 30.0f;
            float DistanceLast = 90.0f;
            float DepthMinFirst = 3.0f;
            float DepthMaxFirst = 8.0f;
            /// The top rung's window is what the lakes hold (map tiles, 2026-10-05, every water sample 15-90 yd from
            /// the spawns): Stonebull's bed is 32.1 yd at its deepest, p90 26.9, p95 28.5; Elune'ara's 61.8, p90
            /// 42.3. [18, 30] is 12% of uniform draws round Stonebull and 8% round Elune'ara, so the placer's 192
            /// tries find one; the first rung's [3, 8] is 7% and 15%.
            float DepthMinLast = 18.0f;
            float DepthMaxLast = 30.0f;
            float Radius = 2.0f;
            float ArriveRise = 3.0f;            // a swimmer over a lakebed marker floats a little above it
            float Drowning = 2.0f;              // per share of maximum health the water took
        } MarkerWater;

        /// The routes stage's markers (MarkerCourse::Routes, M5): one long trip an episode across mixed ground, where
        /// the way is not visible from the start -- round lakes, through canyons, out of a dead end. Planned whole
        /// by the RoutePlanner (TravelPlaceRules::LongRoute) and walked or swum by the controller end to end.
        ///
        /// The ladder (Rungs rungs) moves the trip's straight distance from [NearestFirst, FurthestFirst] to
        /// [NearestLast, FurthestLast] yards and its detour -- the way over the straight line -- from DetourFirst to
        /// DetourLast, each trip within DetourSpan above the rung's and never over DetourCap. Stopped within Radius.
        /// The costs are the ground course's (Stuck, Wall) and the vertical one's (FallDamage). `revisits` counts
        /// RevisitCell-yard cells the seat came back to after RevisitSeconds away: a seat retracing its steps.
        struct MarkerRoutesTuning
        {
            uint32 Rungs = 6;
            float NearestFirst = 150.0f;
            float FurthestFirst = 250.0f;
            float NearestLast = 400.0f;
            float FurthestLast = 600.0f;
            float DetourFirst = 1.3f;
            float DetourLast = 2.4f;
            float DetourSpan = 0.6f;
            float DetourCap = 3.0f;
            float Radius = 2.0f;
            float RevisitCell = 10.0f;
            float RevisitSeconds = 10.0f;
        } MarkerRoutes;

        /// The mounted stage's markers (MarkerCourse::Mounted, M6): a trip worth mounting for. On the ground
        /// (a ride: the route planner's way, the controller's walk of it) the straight distance moves from
        /// [RideNearestFirst, RideFurthestFirst] to [RideNearestLast, RideFurthestLast] yards over the Rungs rungs,
        /// its way at most RideMaxDetour times the line; in a flying arena (ArenaDefinition::Flying, on a map that
        /// flies) from [FlightNearestFirst, FlightFurthestFirst] to [FlightNearestLast, FlightFurthestLast], placed on
        /// ground anywhere -- or, in an air-only arena, only where the ground route does not reach (Travel.AirDetour),
        /// the ground mount masked. Arriving is stopping on it as everywhere: landed, dismounted or not, within Radius.
        /// Mounting is a cast, interrupted by moving and by damage, as a player's is. The costs are the ground and
        /// vertical courses' (Stuck, Wall, FallDamage: a dismount in the air is a fall).
        struct MarkerMountedTuning
        {
            uint32 Rungs = 6;
            float RideNearestFirst = 150.0f;
            float RideFurthestFirst = 250.0f;
            float RideNearestLast = 300.0f;
            float RideFurthestLast = 500.0f;
            float RideMaxDetour = 1.8f;
            float FlightNearestFirst = 200.0f;
            float FlightFurthestFirst = 350.0f;
            float FlightNearestLast = 500.0f;
            float FlightFurthestLast = 900.0f;
            float Radius = 2.5f;
            /// A flight marker has at least this much open sky over it (no overhang, no cave a flyer cannot enter
            /// from above).
            float SkyOpen = 10.0f;
        } MarkerMounted;

        /// The follow stage (Opposition::Follow, M7): keep within [BandMin, BandMax] yards of a moving leader. The
        /// leader walks trips of the rung's length on the ground (TripNearest to the rung's TripFurthest, from
        /// TripFurthestFirst to TripFurthestLast over Rungs rungs), at a walk on the rungs below WalkRungs; from
        /// CastFromRung up a CastShare percent of training episodes give it to a frozen checkpoint (the learner's
        /// cast.agents.leader -- an M6 policy that rides, swims and jumps as it likes) and the rest keep the script.
        /// Evaluations always keep the script: the yardstick does not move.
        ///
        /// Paid to the follower: Kept per second in the band (FollowKept, Outcome), Lost per second past LostYards
        /// (Cost), Aggro per hostile creature newly attacking it (Cost), Progress (Shaping) on closing to the band,
        /// and the ground courses' Stuck, Wall and FallDamage. An episode counts as won on the ladder when the
        /// in-band share is at least WinShare. A catch-up is coming back into the band after CatchUpSeconds out.
        struct FollowTuning
        {
            float BandMin = 3.0f;
            float BandMax = 10.0f;
            float LostYards = 30.0f;
            float Kept = 0.02f;                 // per second in the band
            float Lost = 0.02f;                 // per second past LostYards
            float Aggro = 0.5f;                 // per hostile creature newly attacking the follower
            float Progress = 1.0f;              // over closing LostYards to the band
            uint32 Rungs = 6;
            float TripNearest = 30.0f;
            float TripFurthestFirst = 60.0f;
            float TripFurthestLast = 200.0f;
            uint32 WalkRungs = 2;
            uint32 CastFromRung = 4;
            int32 CastShare = 50;               // percent of training episodes at or above CastFromRung
            float WinShare = 0.8f;
            float CatchUpSeconds = 2.0f;
        } Follow;

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
        /// fade's rung i (scales 1, 0.5, 0.25, 0; SightDraw::WithholdChance): an absent input, never a mask.
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
            float Withhold0 = 0.0f;
            float Withhold1 = 0.25f;
            float Withhold2 = 0.6f;
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

        /// Where death runs on (ArenaDefinition::DeathRuns, DeathBlock): the corpse run. Dying stays costed by the
        /// arena's own death term; nothing here pays for dying.
        struct DeathTuning
        {
            float TimeDead = 0.002f;            // per decision dead or a ghost: the clock keeps running
            float DiedAgain = 3.0f;             // died within DiedAgainMs of rising
            uint32 DiedAgainMs = 30000;
            float SafeRise = 0.5f;              // rose with no hostile creature within its aggro radius + 5 yd
            float SpiritHealer = 1.0f;          // took the spirit healer's resurrection and its sickness
        } Death;

        /// The scripted enemy player of the PvP stage.
        struct OpponentTuning
        {
            int32 LevelSpread = 1;
            uint32 EngageMaxMs = 3000;          // it starts fighting up to this long into the episode
            int32 HealerChance = 20;
            int32 TankChance = 20;
        } Opponent;

        /// Scripted enemy players ambushing the owner (arenas with ambushers). Their class, role and level follow
        /// Opponent.* chances and spread.
        struct AmbushTuning
        {
            uint32 MinMs = 20000;               // beside pulls: they arrive this far into the episode ...
            uint32 MaxMs = 120000;              // ... at the latest
            uint32 EngageMaxMs = 3000;          // they start fighting up to this long after arriving
            float Kill = 3.0f;                  // every seat, per ambusher killed
        } Ambush;

        /// Getting to a place (travel arenas): how far it is, and what arriving pays.
        struct TravelTuning
        {
            float ObjectiveMin = 60.0f;         // ground: yards from the start (by path, reachable on foot)
            float ObjectiveMax = 320.0f;
            /// On foot (ArenaDefinition::OnFoot): shorter, because the lesson is how well the seat covers
            /// ground with what it has rather than whether a ride is worth summoning. Long enough that a
            /// speed cooldown pays for itself and short enough that the trip is not simply a wait.
            /// Inside a building the whole trip is shorter than an outdoor one's first step: an inn is twenty to
            /// thirty yards across, and FootMin alone would put every objective through an outside wall.
            float IndoorMin = 8.0f;
            float IndoorMax = 40.0f;
            float FootMin = 40.0f;
            float FootMax = 160.0f;
            float FlyingMin = 350.0f;           // flying arenas: yards from the start
            float FlyingMax = 700.0f;
            /// Which trips the ground arenas ask for, by how much longer the walking way round is than the
            /// straight line. Drawn uniformly, real detours were the tail -- 51% of stage1_move's trips and 82%
            /// of stage6_travel's had a dry detour under 1.15 -- and a policy taught on straight lines learns to
            /// hold forward. Each episode draws a band first (DetourEasyShare of them under DetourEasy,
            /// DetourMidShare between DetourEasy and DetourHard, the rest from DetourHard up to the generator's
            /// ceiling of 1.8) and looks for an objective in it, settling for any band only once half its
            /// attempts have found nothing. Water, indoor and flying arenas draw no band: each asks for its own
            /// kind of trip.
            float DetourEasy = 1.15f;
            float DetourHard = 1.4f;
            float DetourEasyShare = 0.4f;
            float DetourMidShare = 0.35f;
            /// Air-only arenas (ArenaDefinition::AirOnly): a place is accepted only when the ground route to it
            /// is missing or longer than AirDetour times the straight line, so the wings are the way and not a
            /// slower option; and arriving there means standing within AirArriveRise yards of the objective's
            /// own height, or the foot of the cliff six yards under a plateau's edge would count.
            float AirDetour = 2.5f;
            float AirArriveRise = 10.0f;
            /// Potential shaping: what closing the whole trip pays, spread over its length (per 100 yd on a trip
            /// shorter than that). It used to be per 100 yd whatever the trip, so a 700 yd flight paid 4.3 for
            /// progress against 3.0 for arriving, and rewards.py's own audit said so every twenty-five updates.
            float Progress = 1.0f;
            float Arrive = 3.0f;
            float FastArrive = 6.0f;            // times the fraction of the walk the trip saved (mounting)
            float DamageTaken = 1.0f;           // fraction of the bot's health (falls, what it rode past)
            float Death = 3.0f;
            float StepCost = 0.0002f;           // per decision
            /// Room to move. Charged per second, scaled by how far inside ClearanceMargin the seat is, and
            /// capped per episode at ClearanceMax so it can never approach what arriving is worth (Arrive 3.0).
            /// The margin is deliberately wider than a doorway: the seat should prefer the middle of a corridor,
            /// not refuse a door.
            /// Routing. A route is re-planned when the seat has wandered RouteStray yards from the corner it
            /// was walking to, or when RouteRefresh seconds have passed -- movement first, for the same reason
            /// the ground probe refreshes on movement first. RouteCorner is how near counts as having reached
            /// one, and wants to be wider than a decision's travel (1.75 yd at run speed) so a corner cannot be
            /// stepped over and walked back to.
            float RouteStray = 25.0f;
            float RouteRefresh = 5.0f;
            float RouteCorner = 5.0f;
            float Clearance = 0.08f;            // per second hard against the wall
            float ClearanceMargin = 1.5f;       // yards; closer than this is charged
            float ClearanceMax = 0.6f;          // most an episode may lose to it
            /// Ledge arenas (ArenaDefinition::Ledges): how far the objective is, how far below the seat it sits,
            /// and how much longer the way round on foot has to be than the straight line for the drop to be the
            /// shortcut. LedgeDropMax runs past the lethal fall on purpose: with Slow Fall or Levitate it is free,
            /// without them the seat learns what it costs.
            float LedgeMin = 20.0f;
            float LedgeMax = 120.0f;
            float LedgeDetour = 2.0f;
            float LedgeDropMin = 5.0f;
            float LedgeDropMax = 80.0f;
            /// Dive arenas (ArenaDefinition::Underwater): how far the objective is and how much water stands over
            /// it. DiveDepthMax runs past what one breath reaches on purpose, as LedgeDropMax runs past the lethal
            /// fall: with Unending Breath or Water Breathing the dive is free, without them the seat learns to come
            /// up for air, or what not coming up costs.
            float DiveMin = 20.0f;
            float DiveMax = 120.0f;
            float DiveDepthMin = 6.0f;
            float DiveDepthMax = 40.0f;
            /// Chain arenas (ArenaDefinition::Checkpoints): how far on the next objective is drawn from where the
            /// seat reached the last. Short legs, so a chain of lakebeds is many small dives and the seat is under
            /// water for most of the clock unless it chooses not to be.
            float ChainMin = 30.0f;
            float ChainMax = 60.0f;
        } Travel;

        /// The flag match (Warsong Gulch's rules between two seats).
        struct FlagTuning
        {
            float BaseMin = 100.0f;             // yards between the bases, by path
            float BaseMax = 180.0f;
            uint32 CapturesToWin = 3;
            uint32 RespawnMs = 15000;           // the dead stand up at their base after this (a graveyard wave)
            uint32 DroppedReturnMs = 10000;     // a dropped flag goes home on its own after this
            float TouchDistance = 4.0f;         // yards to pick up, return or capture
            float Capture = 5.0f;
            float Pickup = 1.0f;
            float Return = 1.0f;
            float CarrierKill = 1.5f;           // killing the one carrying the seat's flag
            float Lost = 3.0f;                  // the other side captured the seat's flag
            float Progress = 0.5f;              // potential shaping toward the seat's current objective, per 100 yd
            float Death = 1.0f;
            float StepCost = 0.0002f;           // per decision
        } Flag;

        /// How the scripted players (owner, PvP opponent, ambushers) play.
        struct ScriptedPlayerTuning
        {
            uint32 SpellMinMs = 2000;           // time between damage spells
            uint32 SpellMaxMs = 4000;
            uint32 HealMinMs = 1500;            // time between heals
            uint32 HealMaxMs = 2500;
            uint32 WanderMinMs = 6000;          // between pulls: time between wander steps
            uint32 WanderMaxMs = 12000;
            /// Between pulls, this percent of an owner's steps are a run rather than a wander: a leg at a run to a
            /// point RunMinYards-RunMaxYards from the spawn point (never nearer than RunMinYards to where it stands),
            /// with a real route there. The wander's leash brings it back, another leg. This is where a companion
            /// meets an owner that goes somewhere, which every pull it fights beside is spawned around.
            int32 RunChance = 35;
            float RunMinYards = 40.0f;
            float RunMaxYards = 60.0f;
            float RegenFraction = 0.04f;        // of max health and mana per second, out of combat
            float HealBelow = 0.85f;            // healers heal party members under this health fraction
            float SelfHealBelow = 0.6f;         // PvP healers heal themselves under this
            float HealerRange = 30.0f;          // healers stay this close to the tank
            float TauntRange = 25.0f;
            float RangedMin = 20.0f;            // PvP: a ranged spec backs off inside half this ...
            float RangedMax = 30.0f;            // ... and closes in beyond this
            int32 StealthChance = 50;           // PvP: percent of engagements a rogue (or a feral druid, which
                                                // shifts to Cat Form first) sneaks up in stealth
            int32 TacticsChance = 75;           // PvP: percent of engagements it plays its kit (below)
            uint32 ControlMinMs = 8000;         // ... time between crowd control attempts
            uint32 ControlMaxMs = 15000;
            float DefensiveBelow = 0.35f;       // ... a defensive when its health is under this
            float BreakBelow = 0.6f;            // ... breaks crowd control when its health is under this
        } ScriptedPlayers;

        /// The "human" stand-in seat of the party stages (StandIn.h: its styles, and what each value does). Off
        /// unless StandIn.Share is set.
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

            f("Party.SizeWeight1", tuning.Party.SizeWeight1);
            f("Party.SizeWeight2", tuning.Party.SizeWeight2);
            f("Party.SizeWeight3", tuning.Party.SizeWeight3);
            f("Party.SizeWeight4", tuning.Party.SizeWeight4);
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
            f("Raid.HealthPerGroup", tuning.Raid.HealthPerGroup);
            f("Raid.DamagePerGroup", tuning.Raid.DamagePerGroup);
            f("Raid.TankHold", tuning.Raid.TankHold);
            f("Raid.TankLoose", tuning.Raid.TankLoose);
            f("Raid.TankTarget", tuning.Raid.TankTarget);
            f("Raid.PulledOff", tuning.Raid.PulledOff);
            f("Raid.EarlyPull", tuning.Raid.EarlyPull);
            f("Raid.DrillWeight", tuning.Raid.DrillWeight);
            f("Raid.KeepUp", tuning.Raid.KeepUp);
            f("Raid.Overheal", tuning.Raid.Overheal);
            f("Raid.TankStance", tuning.Raid.TankStance);
            f("Raid.Output", tuning.Raid.Output);
            f("Raid.Idle", tuning.Raid.Idle);
            f("Raid.IdleMs", tuning.Raid.IdleMs);
            f("Raid.IdleReach", tuning.Raid.IdleReach);

            f("Duel.DamageDealt", tuning.Duel.DamageDealt);
            f("Duel.DamageTaken", tuning.Duel.DamageTaken);
            f("Duel.Approach", tuning.Duel.Approach);
            f("Duel.StealthOpener", tuning.Duel.StealthOpener);
            f("Duel.StealthUtility", tuning.Duel.StealthUtility);
            f("Duel.StepCost", tuning.Duel.StepCost);
            f("Duel.Kill", tuning.Duel.Kill);
            f("Duel.FastKill", tuning.Duel.FastKill);
            f("Duel.HealthKept", tuning.Duel.HealthKept);
            f("Duel.Death", tuning.Duel.Death);
            f("Duel.Timeout", tuning.Duel.Timeout);
            f("Duel.TimeoutFloor", tuning.Duel.TimeoutFloor);
            f("Duel.Stall", tuning.Duel.Stall);
            f("Duel.StallGraceMs", tuning.Duel.StallGraceMs);
            f("Duel.PreparationRefundMaxMs", tuning.Duel.PreparationRefundMaxMs);
            f("Duel.Interrupt", tuning.Duel.Interrupt);
            f("Duel.InterruptHeal", tuning.Duel.InterruptHeal);
            f("Duel.InterruptArea", tuning.Duel.InterruptArea);
            f("Duel.InterruptLong", tuning.Duel.InterruptLong);
            f("Duel.Spacing", tuning.Duel.Spacing);
            f("Duel.ShotAtRange", tuning.Duel.ShotAtRange);
            f("Duel.ShotPaused", tuning.Duel.ShotPaused);
            f("Duel.PetTank", tuning.Duel.PetTank);
            f("Duel.PetTankMax", tuning.Duel.PetTankMax);
            f("Duel.MeleeRange", tuning.Duel.MeleeRange);
            f("Duel.RangedRange", tuning.Duel.RangedRange);

            f("Difficulty.MaxTier", tuning.Difficulty.MaxTier);
            f("Difficulty.EliteTier", tuning.Difficulty.EliteTier);
            f("Difficulty.LevelsPerTier", tuning.Difficulty.LevelsPerTier);
            f("Difficulty.RaiseAbove", tuning.Difficulty.RaiseAbove);
            f("Difficulty.LowerBelow", tuning.Difficulty.LowerBelow);
            f("Difficulty.Window", tuning.Difficulty.Window);
            f("Difficulty.ReviewChance", tuning.Difficulty.ReviewChance);
            f("Difficulty.StretchChance", tuning.Difficulty.StretchChance);
            f("Difficulty.CasterChance", tuning.Difficulty.CasterChance);
            f("Difficulty.HazardChance", tuning.Difficulty.HazardChance);
            f("Difficulty.TierScale", tuning.Difficulty.TierScale);

            f("Instance.EngageYards", tuning.Instance.EngageYards);
            f("Instance.TrashRadius", tuning.Instance.TrashRadius);
            f("Instance.MaxTierScale", tuning.Instance.MaxTierScale);
            f("Instance.BossProgress", tuning.Instance.BossProgress);
            f("Instance.Timeout", tuning.Instance.Timeout);
            f("Instance.Stall", tuning.Instance.Stall);
            f("Instance.StallGraceMs", tuning.Instance.StallGraceMs);
            f("Instance.WingTrashKill", tuning.Instance.WingTrashKill);
            f("Instance.WingWaypoint", tuning.Instance.WingWaypoint);
            f("Instance.WingBoss", tuning.Instance.WingBoss);
            f("Instance.WingMidBoss", tuning.Instance.WingMidBoss);
            f("Instance.WingProgress", tuning.Instance.WingProgress);
            f("Instance.WingDeath", tuning.Instance.WingDeath);
            f("Instance.WingWipe", tuning.Instance.WingWipe);
            f("Instance.WingWipes", tuning.Instance.WingWipes);
            f("Instance.WingStall", tuning.Instance.WingStall);
            f("Instance.WingStallOthers", tuning.Instance.WingStallOthers);
            f("Instance.WingEngage", tuning.Instance.WingEngage);
            f("Instance.WingReadyShare", tuning.Instance.WingReadyShare);
            f("Instance.WingStallGraceMs", tuning.Instance.WingStallGraceMs);
            f("Instance.WingTimeout", tuning.Instance.WingTimeout);
            f("Instance.WingWaypointYards", tuning.Instance.WingWaypointYards);
            f("Instance.WingProbe", tuning.Instance.WingProbe);
            f("Instance.WingRungRuns", tuning.Instance.WingRungRuns);
            f("Instance.WingRungTarget", tuning.Instance.WingRungTarget);
            f("Instance.WingRungStart", tuning.Instance.WingRungStart);
            f("Instance.WingSupport", tuning.Instance.WingSupport);
            f("Instance.WingSupplies", tuning.Instance.WingSupplies);
            f("Instance.WingRungFallback", tuning.Instance.WingRungFallback);
            f("Instance.WingInstructHeal", tuning.Instance.WingInstructHeal);
            f("Instance.WingTrace", tuning.Instance.WingTrace);
            f("Instance.WingCrowd", tuning.Instance.WingCrowd);
            f("Instance.WingCrowdFree", tuning.Instance.WingCrowdFree);
            f("Instance.WingFullClear", tuning.Instance.WingFullClear);
            f("Instance.WingRiseMs", tuning.Instance.WingRiseMs);
            f("Instance.WingAutoDoors", tuning.Instance.WingAutoDoors);
            f("Instance.WingStray", tuning.Instance.WingStray);
            f("Instance.WingStrayYards", tuning.Instance.WingStrayYards);
            f("Instance.PullClean", tuning.Instance.PullClean);
            f("Instance.PullExtra", tuning.Instance.PullExtra);
            f("Instance.PullTimeout", tuning.Instance.PullTimeout);
            f("Instance.PullOthers", tuning.Instance.PullOthers);
            f("Instance.PullGraceMs", tuning.Instance.PullGraceMs);
            f("Instance.PullLift", tuning.Instance.PullLift);
            f("Instance.PullStartYards", tuning.Instance.PullStartYards);
            f("Instance.PullRungStart", tuning.Instance.PullRungStart);
            f("Instance.PullRungRuns", tuning.Instance.PullRungRuns);
            f("Instance.PullRungTarget", tuning.Instance.PullRungTarget);
            f("Life.StepCost", tuning.Life.StepCost);
            f("Life.Progress", tuning.Life.Progress);
            f("Life.Wasted", tuning.Life.Wasted);
            f("Life.Death", tuning.Life.Death);
            f("Life.QuestAccepted", tuning.Life.QuestAccepted);
            f("Life.QuestCredit", tuning.Life.QuestCredit);
            f("Life.QuestTurnIn", tuning.Life.QuestTurnIn);
            f("Life.QuestTimeout", tuning.Life.QuestTimeout);
            f("Life.CompleteHeld", tuning.Life.CompleteHeld);
            f("Life.DropRerolls", tuning.Life.DropRerolls);
            f("Life.Poach", tuning.Life.Poach);
            f("Life.ClaimHoldMs", tuning.Life.ClaimHoldMs);
            f("Life.ClaimRadius", tuning.Life.ClaimRadius);
            f("Life.GatherNode", tuning.Life.GatherNode);
            f("Life.GatherSkillUp", tuning.Life.GatherSkillUp);
            f("Life.TownSold", tuning.Life.TownSold);
            f("Life.TownRepaired", tuning.Life.TownRepaired);
            f("Life.TownStocked", tuning.Life.TownStocked);
            f("Life.TownEquipped", tuning.Life.TownEquipped);
            f("Life.TownDone", tuning.Life.TownDone);
            f("Life.SenseRange", tuning.Life.SenseRange);
            f("Life.ObjectiveRadius", tuning.Life.ObjectiveRadius);
            f("Life.ObjectiveSpawns", tuning.Life.ObjectiveSpawns);
            f("Life.NodeRadius", tuning.Life.NodeRadius);
            f("Life.NodeSpawns", tuning.Life.NodeSpawns);
            f("Life.TownRadius", tuning.Life.TownRadius);
            f("Life.TownCopperPerLevelSquared", tuning.Life.TownCopperPerLevelSquared);

            f("Casting.TimeWasted", tuning.Casting.TimeWasted);
            f("Casting.TimeCompleted", tuning.Casting.TimeCompleted);
            f("Casting.Cancel", tuning.Casting.Cancel);

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
            f("Goals.WorldValue", tuning.Goals.WorldValue);
            f("Goals.SecondaryShare", tuning.Goals.SecondaryShare);
            f("Goals.Secondary", tuning.Goals.Secondary);

            f("Order.Focus", tuning.Order.Focus);
            f("Order.PlaceMatch", tuning.Order.PlaceMatch);
            f("Order.PlaceRadius", tuning.Order.PlaceRadius);
            f("Order.PlaceCooldownMs", tuning.Order.PlaceCooldownMs);

            f("Evade.BrokeContact", tuning.Evade.BrokeContact);
            f("Evade.BreakCooldownMs", tuning.Evade.BreakCooldownMs);
            f("Evade.EscapeMs", tuning.Evade.EscapeMs);
            f("Stealth.Stalk", tuning.Stealth.Stalk);
            f("Stealth.StalkYards", tuning.Stealth.StalkYards);
            f("Stealth.StalkMax", tuning.Stealth.StalkMax);
            f("Stealth.OpenerDamage", tuning.Stealth.OpenerDamage);
            f("Stealth.OpenerWindowMs", tuning.Stealth.OpenerWindowMs);

            f("Director.PlaceNearYards", tuning.Director.PlaceNearYards);
            f("Director.PlaceFarYards", tuning.Director.PlaceFarYards);
            f("Director.ClockDecisions", tuning.Director.ClockDecisions);
            f("Director.OrderHoldDecisions", tuning.Director.OrderHoldDecisions);
            f("Director.OrderChange", tuning.Director.OrderChange);
            f("Director.OrderChurn", tuning.Director.OrderChurn);
            f("Director.LowHealth", tuning.Director.LowHealth);

            f("Support.SelfHealing", tuning.Support.SelfHealing);
            f("Support.HealingMana", tuning.Support.HealingMana);
            f("Support.HealingManaWithReadiness", tuning.Support.HealingManaWithReadiness);
            f("Support.BuffCoverage", tuning.Support.BuffCoverage);
            f("Support.PetReady", tuning.Support.PetReady);

            f("Pulls.LinkedChance", tuning.Pulls.LinkedChance);
            f("Pulls.EliteChance", tuning.Pulls.EliteChance);
            f("Pulls.HigherLevelChance", tuning.Pulls.HigherLevelChance);
            f("Pulls.PartyEliteChance", tuning.Pulls.PartyEliteChance);
            f("Pulls.NextPullMinMs", tuning.Pulls.NextPullMinMs);
            f("Pulls.NextPullMaxMs", tuning.Pulls.NextPullMaxMs);
            f("Pulls.OwnerEngageMinMs", tuning.Pulls.OwnerEngageMinMs);
            f("Pulls.OwnerEngageMaxMs", tuning.Pulls.OwnerEngageMaxMs);
            f("Pulls.PartyOwnerEngageMinMs", tuning.Pulls.PartyOwnerEngageMinMs);
            f("Pulls.PartyOwnerEngageMaxMs", tuning.Pulls.PartyOwnerEngageMaxMs);
            f("Pulls.OwnerPullsMinMs", tuning.Pulls.OwnerPullsMinMs);
            f("Pulls.OwnerPullsMaxMs", tuning.Pulls.OwnerPullsMaxMs);
            f("Pulls.OwnerPullsChance", tuning.Pulls.OwnerPullsChance);
            f("Pulls.RecoverFraction", tuning.Pulls.RecoverFraction);
            f("Pulls.DamageDealt", tuning.Pulls.DamageDealt);
            f("Pulls.DamageTaken", tuning.Pulls.DamageTaken);
            f("Pulls.GauntletDamageTaken", tuning.Pulls.GauntletDamageTaken);
            f("Pulls.Approach", tuning.Pulls.Approach);
            f("Pulls.StealthOpener", tuning.Pulls.StealthOpener);
            f("Pulls.StealthUtility", tuning.Pulls.StealthUtility);
            f("Pulls.Interrupt", tuning.Pulls.Interrupt);
            f("Pulls.InterruptHeal", tuning.Pulls.InterruptHeal);
            f("Pulls.InterruptArea", tuning.Pulls.InterruptArea);
            f("Pulls.InterruptLong", tuning.Pulls.InterruptLong);
            f("Pulls.Kill", tuning.Pulls.Kill);
            f("Pulls.StepCost", tuning.Pulls.StepCost);
            f("Pulls.Clear", tuning.Pulls.Clear);
            f("Pulls.FastPull", tuning.Pulls.FastPull);
            f("Pulls.HealthKept", tuning.Pulls.HealthKept);
            f("Pulls.GauntletDeath", tuning.Pulls.GauntletDeath);
            f("Pulls.PackClear", tuning.Pulls.PackClear);
            f("Pulls.FastClear", tuning.Pulls.FastClear);
            f("Pulls.PackHealthKept", tuning.Pulls.PackHealthKept);
            f("Pulls.PackDeath", tuning.Pulls.PackDeath);
            f("Pulls.MaxTier", tuning.Pulls.MaxTier);
            f("Pulls.Timeout", tuning.Pulls.Timeout);
            f("Pulls.TimeoutFloor", tuning.Pulls.TimeoutFloor);
            f("Pulls.Overtime", tuning.Pulls.Overtime);
            f("Pulls.OvertimeGraceMs", tuning.Pulls.OvertimeGraceMs);
            f("Pulls.SinglePackControl", tuning.Pulls.SinglePackControl);
            f("Pulls.SinglePackControlMax", tuning.Pulls.SinglePackControlMax);
            f("Pulls.ControlHealthFloor", tuning.Pulls.ControlHealthFloor);
            f("Pulls.ControlFallbackDps", tuning.Pulls.ControlFallbackDps);
            f("Pulls.ControlRateMinMs", tuning.Pulls.ControlRateMinMs);
            f("Pulls.ControlGraceMaxMs", tuning.Pulls.ControlGraceMaxMs);
            f("Pulls.Stall", tuning.Pulls.Stall);
            f("Pulls.StallGraceMs", tuning.Pulls.StallGraceMs);
            f("Pulls.PreparationRefundMaxMs", tuning.Pulls.PreparationRefundMaxMs);
            f("Pulls.Spacing", tuning.Pulls.Spacing);
            f("Pulls.CampExtraPack", tuning.Pulls.CampExtraPack);
            f("Pulls.CampCleanPack", tuning.Pulls.CampCleanPack);
            f("Pulls.CampRestMs", tuning.Pulls.CampRestMs);
            f("Pulls.SoloGauntletClear", tuning.Pulls.SoloGauntletClear);
            f("Pulls.SoloGauntletFastPull", tuning.Pulls.SoloGauntletFastPull);
            f("Pulls.SoloGauntletHealthKept", tuning.Pulls.SoloGauntletHealthKept);
            f("Pulls.SoloGauntletDeath", tuning.Pulls.SoloGauntletDeath);
            f("Pulls.SoloGauntletReadiness", tuning.Pulls.SoloGauntletReadiness);
            f("Pulls.SoloGauntletWinPulls", tuning.Pulls.SoloGauntletWinPulls);
            f("Pulls.GauntletSupplies", tuning.Pulls.GauntletSupplies);
            f("Pulls.SoloGauntletControl", tuning.Pulls.SoloGauntletControl);
            f("Pulls.SoloGauntletControlMax", tuning.Pulls.SoloGauntletControlMax);
            f("Pulls.ArriveMinMs", tuning.Pulls.ArriveMinMs);
            f("Pulls.ArriveMaxMs", tuning.Pulls.ArriveMaxMs);
            f("Pulls.ArriveShrinkMs", tuning.Pulls.ArriveShrinkMs);
            f("Pulls.ArriveFloorMs", tuning.Pulls.ArriveFloorMs);
            f("Pulls.NextPullShrinkMs", tuning.Pulls.NextPullShrinkMs);
            f("Pulls.NextPullFloorMs", tuning.Pulls.NextPullFloorMs);
            f("Pulls.GauntletDenseScale", tuning.Pulls.GauntletDenseScale);
            f("Pulls.OwnerClearScale", tuning.Pulls.OwnerClearScale);
            f("Pulls.OwnerReadiness", tuning.Pulls.OwnerReadiness);
            f("Pulls.OwnerControl", tuning.Pulls.OwnerControl);
            f("Pulls.OwnerControlMax", tuning.Pulls.OwnerControlMax);
            f("Pulls.OwnerWinPulls", tuning.Pulls.OwnerWinPulls);

            f("Options.RestMaxMs", tuning.Options.RestMaxMs);
            f("Options.HoldInterruptMs", tuning.Options.HoldInterruptMs);
            f("Hazards.Damage", tuning.Hazards.Damage);
            f("Hazards.Standing", tuning.Hazards.Standing);
            f("Hazards.Max", tuning.Hazards.Max);
            f("Dummy.HealthScale", tuning.Dummy.HealthScale);
            f("Dummy.HittingHealthScale", tuning.Dummy.HittingHealthScale);
            f("Dummy.Damage", tuning.Dummy.Damage);
            f("Dummy.Kill", tuning.Dummy.Kill);
            f("Dummy.Death", tuning.Dummy.Death);
            f("Dummy.Bleed", tuning.Dummy.Bleed);
            f("Dummy.Hurt", tuning.Dummy.Hurt);
            f("Dummy.Resource", tuning.Dummy.Resource);
            f("Dummy.AddEveryMs", tuning.Dummy.AddEveryMs);
            f("Dummy.MaxAdds", tuning.Dummy.MaxAdds);
            f("Markers.Arrive", tuning.Markers.Arrive);
            f("Markers.StepCost", tuning.Markers.StepCost);
            f("Markers.Death", tuning.Markers.Death);
            f("Markers.Progress", tuning.Markers.Progress);
            f("Markers.Facing", tuning.Markers.Facing);
            f("Markers.StopMoved", tuning.Markers.StopMoved);
            f("Markers.MarkersMin", tuning.Markers.MarkersMin);
            f("Markers.MarkersMax", tuning.Markers.MarkersMax);
            f("Markers.Rungs", tuning.Markers.Rungs);
            f("Markers.DistanceMin", tuning.Markers.DistanceMin);
            f("Markers.DistanceFirst", tuning.Markers.DistanceFirst);
            f("Markers.DistanceLast", tuning.Markers.DistanceLast);
            f("Markers.BearingFirst", tuning.Markers.BearingFirst);
            f("Markers.BearingLast", tuning.Markers.BearingLast);
            f("Markers.RadiusFirst", tuning.Markers.RadiusFirst);
            f("Markers.RadiusLast", tuning.Markers.RadiusLast);
            f("Markers.MaxDetour", tuning.Markers.MaxDetour);
            f("Markers.StopNear", tuning.Markers.StopNear);
            f("Markers.ArriveRise", tuning.Markers.ArriveRise);
            f("Markers.RouteMaxDrop", tuning.Markers.RouteMaxDrop);
            f("Markers.FallbackCeiling", tuning.Markers.FallbackCeiling);
            f("Markers.FallbackMinLegs", tuning.Markers.FallbackMinLegs);
            f("MarkerGround.MarkersMin", tuning.MarkerGround.MarkersMin);
            f("MarkerGround.MarkersMax", tuning.MarkerGround.MarkersMax);
            f("MarkerGround.Rungs", tuning.MarkerGround.Rungs);
            f("MarkerGround.DistanceMin", tuning.MarkerGround.DistanceMin);
            f("MarkerGround.DistanceFirst", tuning.MarkerGround.DistanceFirst);
            f("MarkerGround.DistanceLast", tuning.MarkerGround.DistanceLast);
            f("MarkerGround.DetourFirst", tuning.MarkerGround.DetourFirst);
            f("MarkerGround.DetourLast", tuning.MarkerGround.DetourLast);
            f("MarkerGround.DetourSpan", tuning.MarkerGround.DetourSpan);
            f("MarkerGround.Radius", tuning.MarkerGround.Radius);
            f("MarkerGround.Stuck", tuning.MarkerGround.Stuck);
            f("MarkerGround.Wall", tuning.MarkerGround.Wall);
            f("MarkerGround.WallSlide", tuning.MarkerGround.WallSlide);
            f("MarkerVertical.MarkersMin", tuning.MarkerVertical.MarkersMin);
            f("MarkerVertical.MarkersMax", tuning.MarkerVertical.MarkersMax);
            f("MarkerVertical.Rungs", tuning.MarkerVertical.Rungs);
            f("MarkerVertical.DistanceMin", tuning.MarkerVertical.DistanceMin);
            f("MarkerVertical.DistanceFirst", tuning.MarkerVertical.DistanceFirst);
            f("MarkerVertical.DistanceLast", tuning.MarkerVertical.DistanceLast);
            f("MarkerVertical.HeightMinFirst", tuning.MarkerVertical.HeightMinFirst);
            f("MarkerVertical.HeightMaxFirst", tuning.MarkerVertical.HeightMaxFirst);
            f("MarkerVertical.HeightMinLast", tuning.MarkerVertical.HeightMinLast);
            f("MarkerVertical.HeightMaxLast", tuning.MarkerVertical.HeightMaxLast);
            f("MarkerVertical.Radius", tuning.MarkerVertical.Radius);
            f("MarkerVertical.SafeDrop", tuning.MarkerVertical.SafeDrop);
            f("MarkerVertical.FallDamage", tuning.MarkerVertical.FallDamage);
            f("MarkerVertical.RoomUpShare", tuning.MarkerVertical.RoomUpShare);
            f("MarkerVertical.UpstairsRise", tuning.MarkerVertical.UpstairsRise);
            f("MarkerWater.MarkersMin", tuning.MarkerWater.MarkersMin);
            f("MarkerWater.MarkersMax", tuning.MarkerWater.MarkersMax);
            f("MarkerWater.ChainMin", tuning.MarkerWater.ChainMin);
            f("MarkerWater.ChainMax", tuning.MarkerWater.ChainMax);
            f("MarkerWater.Rungs", tuning.MarkerWater.Rungs);
            f("MarkerWater.DistanceMin", tuning.MarkerWater.DistanceMin);
            f("MarkerWater.DistanceFirst", tuning.MarkerWater.DistanceFirst);
            f("MarkerWater.DistanceLast", tuning.MarkerWater.DistanceLast);
            f("MarkerWater.DepthMinFirst", tuning.MarkerWater.DepthMinFirst);
            f("MarkerWater.DepthMaxFirst", tuning.MarkerWater.DepthMaxFirst);
            f("MarkerWater.DepthMinLast", tuning.MarkerWater.DepthMinLast);
            f("MarkerWater.DepthMaxLast", tuning.MarkerWater.DepthMaxLast);
            f("MarkerWater.Radius", tuning.MarkerWater.Radius);
            f("MarkerWater.ArriveRise", tuning.MarkerWater.ArriveRise);
            f("MarkerWater.Drowning", tuning.MarkerWater.Drowning);
            f("MarkerRoutes.Rungs", tuning.MarkerRoutes.Rungs);
            f("MarkerRoutes.NearestFirst", tuning.MarkerRoutes.NearestFirst);
            f("MarkerRoutes.FurthestFirst", tuning.MarkerRoutes.FurthestFirst);
            f("MarkerRoutes.NearestLast", tuning.MarkerRoutes.NearestLast);
            f("MarkerRoutes.FurthestLast", tuning.MarkerRoutes.FurthestLast);
            f("MarkerRoutes.DetourFirst", tuning.MarkerRoutes.DetourFirst);
            f("MarkerRoutes.DetourLast", tuning.MarkerRoutes.DetourLast);
            f("MarkerRoutes.DetourSpan", tuning.MarkerRoutes.DetourSpan);
            f("MarkerRoutes.DetourCap", tuning.MarkerRoutes.DetourCap);
            f("MarkerRoutes.Radius", tuning.MarkerRoutes.Radius);
            f("MarkerRoutes.RevisitCell", tuning.MarkerRoutes.RevisitCell);
            f("MarkerRoutes.RevisitSeconds", tuning.MarkerRoutes.RevisitSeconds);
            f("MarkerMounted.Rungs", tuning.MarkerMounted.Rungs);
            f("MarkerMounted.RideNearestFirst", tuning.MarkerMounted.RideNearestFirst);
            f("MarkerMounted.RideFurthestFirst", tuning.MarkerMounted.RideFurthestFirst);
            f("MarkerMounted.RideNearestLast", tuning.MarkerMounted.RideNearestLast);
            f("MarkerMounted.RideFurthestLast", tuning.MarkerMounted.RideFurthestLast);
            f("MarkerMounted.RideMaxDetour", tuning.MarkerMounted.RideMaxDetour);
            f("MarkerMounted.FlightNearestFirst", tuning.MarkerMounted.FlightNearestFirst);
            f("MarkerMounted.FlightFurthestFirst", tuning.MarkerMounted.FlightFurthestFirst);
            f("MarkerMounted.FlightNearestLast", tuning.MarkerMounted.FlightNearestLast);
            f("MarkerMounted.FlightFurthestLast", tuning.MarkerMounted.FlightFurthestLast);
            f("MarkerMounted.Radius", tuning.MarkerMounted.Radius);
            f("MarkerMounted.SkyOpen", tuning.MarkerMounted.SkyOpen);
            f("Follow.BandMin", tuning.Follow.BandMin);
            f("Follow.BandMax", tuning.Follow.BandMax);
            f("Follow.LostYards", tuning.Follow.LostYards);
            f("Follow.Kept", tuning.Follow.Kept);
            f("Follow.Lost", tuning.Follow.Lost);
            f("Follow.Aggro", tuning.Follow.Aggro);
            f("Follow.Progress", tuning.Follow.Progress);
            f("Follow.Rungs", tuning.Follow.Rungs);
            f("Follow.TripNearest", tuning.Follow.TripNearest);
            f("Follow.TripFurthestFirst", tuning.Follow.TripFurthestFirst);
            f("Follow.TripFurthestLast", tuning.Follow.TripFurthestLast);
            f("Follow.WalkRungs", tuning.Follow.WalkRungs);
            f("Follow.CastFromRung", tuning.Follow.CastFromRung);
            f("Follow.CastShare", tuning.Follow.CastShare);
            f("Follow.WinShare", tuning.Follow.WinShare);
            f("Follow.CatchUpSeconds", tuning.Follow.CatchUpSeconds);
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
            f("Options.JitterDecayMs", tuning.Options.JitterDecayMs);
            f("Owner.LevelSpread", tuning.Owner.LevelSpread);
            f("Owner.TankChance", tuning.Owner.TankChance);
            f("Owner.HealerChance", tuning.Owner.HealerChance);
            f("Owner.CastScriptedShare", tuning.Owner.CastScriptedShare);
            f("Owner.DamageTakenDps", tuning.Owner.DamageTakenDps);
            f("Owner.DamageTakenProtector", tuning.Owner.DamageTakenProtector);
            f("Owner.TankOwnerDamageShare", tuning.Owner.TankOwnerDamageShare);
            f("Owner.Healing", tuning.Owner.Healing);
            f("Owner.TankDamageRefund", tuning.Owner.TankDamageRefund);
            f("Owner.TankHold", tuning.Owner.TankHold);
            f("Owner.TankLose", tuning.Owner.TankLose);
            f("Owner.PulledThreat", tuning.Owner.PulledThreat);
            f("Owner.SoloFight", tuning.Owner.SoloFight);
            f("Owner.FollowFar", tuning.Owner.FollowFar);
            f("Owner.FollowNear", tuning.Owner.FollowNear);
            f("Owner.FollowTrail", tuning.Owner.FollowTrail);
            f("Owner.FollowFarDistance", tuning.Owner.FollowFarDistance);
            f("Owner.FollowNearDistance", tuning.Owner.FollowNearDistance);
            f("Owner.Death", tuning.Owner.Death);

            f("Resurrection.GraceMs", tuning.Resurrection.GraceMs);
            f("Resurrection.ReviveAlly", tuning.Resurrection.ReviveAlly);
            f("Output.Clock", tuning.Output.Clock);
            f("Death.TimeDead", tuning.Death.TimeDead);
            f("Death.DiedAgain", tuning.Death.DiedAgain);
            f("Death.DiedAgainMs", tuning.Death.DiedAgainMs);
            f("Death.SafeRise", tuning.Death.SafeRise);
            f("Death.SpiritHealer", tuning.Death.SpiritHealer);

            f("Opponent.LevelSpread", tuning.Opponent.LevelSpread);
            f("Opponent.EngageMaxMs", tuning.Opponent.EngageMaxMs);
            f("Opponent.HealerChance", tuning.Opponent.HealerChance);
            f("Opponent.TankChance", tuning.Opponent.TankChance);

            f("Ambush.MinMs", tuning.Ambush.MinMs);
            f("Ambush.MaxMs", tuning.Ambush.MaxMs);
            f("Ambush.EngageMaxMs", tuning.Ambush.EngageMaxMs);
            f("Ambush.Kill", tuning.Ambush.Kill);

            f("Travel.ObjectiveMin", tuning.Travel.ObjectiveMin);
            f("Travel.ObjectiveMax", tuning.Travel.ObjectiveMax);
            f("Travel.FootMin", tuning.Travel.FootMin);
            f("Travel.IndoorMin", tuning.Travel.IndoorMin);
            f("Travel.IndoorMax", tuning.Travel.IndoorMax);
            f("Travel.FootMax", tuning.Travel.FootMax);
            f("Travel.FlyingMin", tuning.Travel.FlyingMin);
            f("Travel.FlyingMax", tuning.Travel.FlyingMax);
            f("Travel.DetourEasy", tuning.Travel.DetourEasy);
            f("Travel.DetourHard", tuning.Travel.DetourHard);
            f("Travel.DetourEasyShare", tuning.Travel.DetourEasyShare);
            f("Travel.DetourMidShare", tuning.Travel.DetourMidShare);
            f("Travel.AirDetour", tuning.Travel.AirDetour);
            f("Travel.AirArriveRise", tuning.Travel.AirArriveRise);
            f("Travel.Progress", tuning.Travel.Progress);
            f("Travel.Arrive", tuning.Travel.Arrive);
            f("Travel.FastArrive", tuning.Travel.FastArrive);
            f("Travel.DamageTaken", tuning.Travel.DamageTaken);
            f("Travel.Death", tuning.Travel.Death);
            f("Travel.StepCost", tuning.Travel.StepCost);
            f("Travel.RouteStray", tuning.Travel.RouteStray);
            f("Travel.RouteRefresh", tuning.Travel.RouteRefresh);
            f("Travel.RouteCorner", tuning.Travel.RouteCorner);
            f("Travel.Clearance", tuning.Travel.Clearance);
            f("Travel.ClearanceMargin", tuning.Travel.ClearanceMargin);
            f("Travel.ClearanceMax", tuning.Travel.ClearanceMax);
            f("Travel.LedgeMin", tuning.Travel.LedgeMin);
            f("Travel.LedgeMax", tuning.Travel.LedgeMax);
            f("Travel.LedgeDetour", tuning.Travel.LedgeDetour);
            f("Travel.LedgeDropMin", tuning.Travel.LedgeDropMin);
            f("Travel.LedgeDropMax", tuning.Travel.LedgeDropMax);
            f("Travel.DiveMin", tuning.Travel.DiveMin);
            f("Travel.DiveMax", tuning.Travel.DiveMax);
            f("Travel.DiveDepthMin", tuning.Travel.DiveDepthMin);
            f("Travel.DiveDepthMax", tuning.Travel.DiveDepthMax);
            f("Travel.ChainMin", tuning.Travel.ChainMin);
            f("Travel.ChainMax", tuning.Travel.ChainMax);

            f("Flag.BaseMin", tuning.Flag.BaseMin);
            f("Flag.BaseMax", tuning.Flag.BaseMax);
            f("Flag.CapturesToWin", tuning.Flag.CapturesToWin);
            f("Flag.RespawnMs", tuning.Flag.RespawnMs);
            f("Flag.DroppedReturnMs", tuning.Flag.DroppedReturnMs);
            f("Flag.TouchDistance", tuning.Flag.TouchDistance);
            f("Flag.Capture", tuning.Flag.Capture);
            f("Flag.Pickup", tuning.Flag.Pickup);
            f("Flag.Return", tuning.Flag.Return);
            f("Flag.CarrierKill", tuning.Flag.CarrierKill);
            f("Flag.Lost", tuning.Flag.Lost);
            f("Flag.Progress", tuning.Flag.Progress);
            f("Flag.Death", tuning.Flag.Death);
            f("Flag.StepCost", tuning.Flag.StepCost);

            f("ScriptedPlayers.SpellMinMs", tuning.ScriptedPlayers.SpellMinMs);
            f("ScriptedPlayers.SpellMaxMs", tuning.ScriptedPlayers.SpellMaxMs);
            f("ScriptedPlayers.HealMinMs", tuning.ScriptedPlayers.HealMinMs);
            f("ScriptedPlayers.HealMaxMs", tuning.ScriptedPlayers.HealMaxMs);
            f("ScriptedPlayers.WanderMinMs", tuning.ScriptedPlayers.WanderMinMs);
            f("ScriptedPlayers.WanderMaxMs", tuning.ScriptedPlayers.WanderMaxMs);
            f("ScriptedPlayers.RunChance", tuning.ScriptedPlayers.RunChance);
            f("ScriptedPlayers.RunMinYards", tuning.ScriptedPlayers.RunMinYards);
            f("ScriptedPlayers.RunMaxYards", tuning.ScriptedPlayers.RunMaxYards);
            f("ScriptedPlayers.RegenFraction", tuning.ScriptedPlayers.RegenFraction);
            f("ScriptedPlayers.HealBelow", tuning.ScriptedPlayers.HealBelow);
            f("ScriptedPlayers.SelfHealBelow", tuning.ScriptedPlayers.SelfHealBelow);
            f("ScriptedPlayers.HealerRange", tuning.ScriptedPlayers.HealerRange);
            f("ScriptedPlayers.TauntRange", tuning.ScriptedPlayers.TauntRange);
            f("ScriptedPlayers.RangedMin", tuning.ScriptedPlayers.RangedMin);
            f("ScriptedPlayers.RangedMax", tuning.ScriptedPlayers.RangedMax);
            f("ScriptedPlayers.StealthChance", tuning.ScriptedPlayers.StealthChance);
            f("ScriptedPlayers.TacticsChance", tuning.ScriptedPlayers.TacticsChance);
            f("ScriptedPlayers.ControlMinMs", tuning.ScriptedPlayers.ControlMinMs);
            f("ScriptedPlayers.ControlMaxMs", tuning.ScriptedPlayers.ControlMaxMs);
            f("ScriptedPlayers.DefensiveBelow", tuning.ScriptedPlayers.DefensiveBelow);
            f("ScriptedPlayers.BreakBelow", tuning.ScriptedPlayers.BreakBelow);

            f("StandIn.Share", tuning.StandIn.Share);
            f("StandIn.LeadChance", tuning.StandIn.LeadChance);
            f("StandIn.TankChance", tuning.StandIn.TankChance);
            f("StandIn.HealerChance", tuning.StandIn.HealerChance);
            f("StandIn.SlowChance", tuning.StandIn.SlowChance);
            f("StandIn.PullEarlyChance", tuning.StandIn.PullEarlyChance);
            f("StandIn.PullEarlyPerMinute", tuning.StandIn.PullEarlyPerMinute);
            f("StandIn.PullEarlyMinMs", tuning.StandIn.PullEarlyMinMs);
            f("StandIn.PullEarlyMaxMs", tuning.StandIn.PullEarlyMaxMs);
            f("StandIn.WanderChance", tuning.StandIn.WanderChance);
            f("StandIn.WanderPerMinute", tuning.StandIn.WanderPerMinute);
            f("StandIn.WanderMinMs", tuning.StandIn.WanderMinMs);
            f("StandIn.WanderMaxMs", tuning.StandIn.WanderMaxMs);
            f("StandIn.RestChance", tuning.StandIn.RestChance);
            f("StandIn.RestPerMinute", tuning.StandIn.RestPerMinute);
            f("StandIn.RestMinMs", tuning.StandIn.RestMinMs);
            f("StandIn.RestMaxMs", tuning.StandIn.RestMaxMs);
            f("StandIn.LagChance", tuning.StandIn.LagChance);
            f("StandIn.LagPerMinute", tuning.StandIn.LagPerMinute);
            f("StandIn.LagMinMs", tuning.StandIn.LagMinMs);
            f("StandIn.LagMaxMs", tuning.StandIn.LagMaxMs);
            f("StandIn.AfkChance", tuning.StandIn.AfkChance);
            f("StandIn.AfkPerMinute", tuning.StandIn.AfkPerMinute);
            f("StandIn.AfkMinMs", tuning.StandIn.AfkMinMs);
            f("StandIn.AfkMaxMs", tuning.StandIn.AfkMaxMs);
            f("StandIn.RestBelow", tuning.StandIn.RestBelow);
            f("StandIn.FollowYards", tuning.StandIn.FollowYards);
            f("StandIn.LagYards", tuning.StandIn.LagYards);
            f("StandIn.WanderMinYards", tuning.StandIn.WanderMinYards);
            f("StandIn.WanderMaxYards", tuning.StandIn.WanderMaxYards);
            f("StandIn.MeleeYards", tuning.StandIn.MeleeYards);
            f("StandIn.RangedYards", tuning.StandIn.RangedYards);
            f("StandIn.HealerYards", tuning.StandIn.HealerYards);
            f("StandIn.FastWaitMs", tuning.StandIn.FastWaitMs);
            f("StandIn.SlowWaitMs", tuning.StandIn.SlowWaitMs);
            f("StandIn.FastReactMs", tuning.StandIn.FastReactMs);
            f("StandIn.SlowReactMs", tuning.StandIn.SlowReactMs);
        }

        /// The values of the config keys <prefix><key>, each defaulting to the value above; min/max pairs are
        /// ordered.
        [[nodiscard]] static CurriculumTuning Load(std::string const& prefix);

        /// Every value as a JSON object, keys as in the config.
        [[nodiscard]] boost::json::object Json() const;
    };
}

#endif
