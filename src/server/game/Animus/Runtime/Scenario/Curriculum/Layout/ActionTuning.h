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

#ifndef ANIMUS_LIB_CURRICULUM_LAYOUT_ACTION_TUNING_H
#define ANIMUS_LIB_CURRICULUM_LAYOUT_ACTION_TUNING_H

#include "Define.h"

/*
 * The two slices of the curriculum tuning the runtime reads while a seat plays: how often an action may be pressed
 * (ActionTuning) and how long a durative action may run (OptionTuning). They change what the policy may press, so they
 * must match what it trained with. Plain value structs with no training dependency; CurriculumTuning (training) holds
 * one of each as Actions and Options and loads them from the host's config keys.
 */
namespace Animus::Curriculum
{
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
    };

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
    };
}

#endif
