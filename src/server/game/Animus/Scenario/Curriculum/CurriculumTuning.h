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
            float TankLoseTeammate = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
            float TeammateDeath = 3.0f;
        } Party;

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
            float Spacing = 0.03f;
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
            /// Paid on a wipe or an evade for the share of the boss's health the fight took off it, so a forty-seat
            /// fight has a gradient before its first kill: at 5, a wipe at 40% pays 3 (x the tier scale).
            float BossProgress = 5.0f;
            float Timeout = 10.0f;              // the clock, scaled by what is left of the boss (Duel.TimeoutFloor)
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
            float QuestCredit = 3.0f;           // spread over the objectives' counts, times the tier scale
            float QuestTurnIn = 10.0f;          // times the tier scale
            float QuestTimeout = 3.0f;          // the clock without a turn-in, less what was done, over the tier scale
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
            /// Paid once for each goal the seat holds, on the first decision it holds it (StageScenario::GoalHeld),
            /// not per decision: a goal is there to be reached, and paying to sit in one made standing at range the
            /// stage's second largest earner. Small on purpose: it is there to keep the goals apart -- without it
            /// nothing stops every goal collapsing into one -- not to pay for play the stage's own terms price.
            float Match = 0.02f;
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
        } Stealth;

        /// What the director's own calls mean in yards.
        struct DirectorTuning
        {
            /// How far a place sits off its anchor, at each ring. The whole point of naming a ring rather
            /// than a distance is that these two numbers are all that changes between an arena and a
            /// continent: the thirteen place actions mean the same thing at any scale.
            float PlaceNearYards = 20.0f;
            float PlaceFarYards = 60.0f;
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
            float Repeat = 0.02f;
            uint32 RepeatWindowMs = 10000;
            uint32 RepeatFree = 3;              // presses of one action within the window that cost nothing
            /// How far below where a jump would come down the ground is looked for before the jump is refused.
            /// The only limit on a drop: a landing this deep is a fall the seat can choose, and what it costs --
            /// nothing with Slow Fall, health past fourteen yards, death past about seventy -- is the seat's to
            /// learn from OBS_JUMP_DROP and from what happens. Only the void is masked.
            float JumpDropSearch = 200.0f;
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
            float Spacing = 0.03f;              // pack: per second a ranged spec is hit in melee reach
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
            float OwnerReadiness = 0.5f;
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
            float Healing = 2.0f;               // any role: effective healing and protection on the owner, as a
                                                // fraction of its health
            float TankDamageRefund = 0.5f;      // tanks: soften the pulls' damage taken
            float TankHold = 0.002f;            // tanks: per enemy on the tank, per decision
            float TankLose = 0.02f;             // tanks: per enemy on the owner, per decision
            float PulledThreat = 0.004f;        // damage dealers and healers beside a TANK owner: per enemy on
                                                // the bot, per decision; not charged beside any other owner
            float SoloFight = 0.01f;            // per decision in combat while the owner is not
            float FollowFar = 0.002f;           // per decision out of combat beyond FollowFarDistance
            float FollowNear = 0.0005f;         // per decision out of combat within FollowNearDistance
            float FollowFarDistance = 25.0f;
            float FollowNearDistance = 12.0f;
            float Death = 15.0f;                // per owner death, every seat: more than the seat's own (GauntletDeath)
        } Owner;

        /// Durative actions (SeatOption): how long each may run before the seat has to choose again. They end on
        /// their own conditions too, and any other action the policy takes cancels them.
        struct OptionTuning
        {
            uint32 RestMaxMs = 30000;           // eat and drink until health and mana are back
            uint32 HoldInterruptMs = 10000;     // interrupt the target as soon as it casts
            /// How long a chosen bearing keeps being walked before it lapses (MoveBlock). Shorter than the two
            /// above on purpose: resting and holding an interrupt are standing instructions that stay true while
            /// the fight does, where a direction chosen against the ground goes stale as soon as the seat has
            /// covered it. The policy re-presses to keep going, which is what a held key is.
            uint32 MoveBearingMs = 3000;
            /// How long a turn or a pitch keeps being held. Shorter again than a bearing: a seat that keeps turning
            /// for three seconds has spun round twice, so this is the length of a glance rather than of a journey.
            /// The policy re-presses to keep turning, and what it has turned to is kept when it stops.
            uint32 MoveTurnMs = 750;
            uint32 MovePitchMs = 750;
            /// How long a companion's follow keeps after the owner before it lapses (CompanionBlock). Longer than a
            /// bearing: where the owner is going is the owner's to know, and a follow that ends every three seconds
            /// behind a running owner is three seconds of re-pressing for nothing chosen. Ends on its own when the
            /// seat is there and the owner has stopped.
            uint32 FollowMs = 6000;
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

        /// Resurrecting: a seat's own Soulstone or Reincarnation, and revives on allies (companion and party stages).
        struct ResurrectionTuning
        {
            uint32 GraceMs = 20000;             // the dead wait this long for a resurrection they can get before solo
                                                // stages end and owner stages stand them up (the next pull waits too)
            float ReviveAlly = 1.5f;            // a dead ally the seat resurrected stood up
        } Resurrection;

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
            f("Party.TankLoseTeammate", tuning.Party.TankLoseTeammate);
            f("Party.TeammateDeath", tuning.Party.TeammateDeath);

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
            f("Life.StepCost", tuning.Life.StepCost);
            f("Life.Progress", tuning.Life.Progress);
            f("Life.Wasted", tuning.Life.Wasted);
            f("Life.Death", tuning.Life.Death);
            f("Life.QuestAccepted", tuning.Life.QuestAccepted);
            f("Life.QuestCredit", tuning.Life.QuestCredit);
            f("Life.QuestTurnIn", tuning.Life.QuestTurnIn);
            f("Life.QuestTimeout", tuning.Life.QuestTimeout);
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
            f("Actions.JumpDropSearch", tuning.Actions.JumpDropSearch);

            f("Goals.Match", tuning.Goals.Match);

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

            f("Director.PlaceNearYards", tuning.Director.PlaceNearYards);
            f("Director.PlaceFarYards", tuning.Director.PlaceFarYards);

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
            f("Options.MoveBearingMs", tuning.Options.MoveBearingMs);
            f("Options.MoveTurnMs", tuning.Options.MoveTurnMs);
            f("Options.MovePitchMs", tuning.Options.MovePitchMs);
            f("Options.FollowMs", tuning.Options.FollowMs);
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
            f("Owner.FollowFarDistance", tuning.Owner.FollowFarDistance);
            f("Owner.FollowNearDistance", tuning.Owner.FollowNearDistance);
            f("Owner.Death", tuning.Owner.Death);

            f("Resurrection.GraceMs", tuning.Resurrection.GraceMs);
            f("Resurrection.ReviveAlly", tuning.Resurrection.ReviveAlly);

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
        }

        /// The values of the config keys <prefix><key>, each defaulting to the value above; min/max pairs are
        /// ordered.
        [[nodiscard]] static CurriculumTuning Load(std::string const& prefix);

        /// Every value as a JSON object, keys as in the config.
        [[nodiscard]] boost::json::object Json() const;
    };
}

#endif
