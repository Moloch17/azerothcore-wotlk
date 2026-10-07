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

#ifndef ANIMUS_LIB_CURRICULUM_STAND_IN_H
#define ANIMUS_LIB_CURRICULUM_STAND_IN_H

#include "Seek.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

/// **The "human" stand-in** (dungeon-curriculum plan I7): one party seat played by a script that plays like a person
/// rather than like a bot, so the learned seats learn to work with whoever joins them -- the end use is a human
/// collaborator in any role -- and not only with copies of themselves.
///
/// **Its style**, drawn once an episode from a seed (Draw), so an evaluation meets the same people every time:
/// - **leads or follows.** A leader goes to the next pull and starts it; a follower keeps near the party's leader. A
///   leading stand-in sits in seat 0, which PartyEncounter makes the group's leader (StageScenario::DrawStandIn);
/// - **a role** -- tank, healer or damage -- within what its class and build can do (RoleFor, read off the seat's
///   Aptitude): a priest wanting to tank deals damage instead. The seats left to the learned party fill the rest;
/// - **a pace**: fast chains pulls with a short breather and reacts at once; slow waits longer between pulls and
///   takes a moment to react when a fight starts;
/// - **quirks**, each in a share of the styles (Tuning::*Chance), firing at a rate while their moment lasts
///   (Tuning::*PerMinute) for a drawn while (Tuning::*MinMs-*MaxMs):
///   - **pulls early**: out of combat, it goes for the next pull before the party is ready;
///   - **wanders**: out of combat, it walks off a little way to look at something;
///   - **rests**: out of combat, it stops and sits for a while -- always when its health or mana is low;
///   - **lags**: a follower drifts well behind the party for a while;
///   - **goes AFK**: out of combat, it lets go of everything and does nothing for a while, and stays away even if a
///     fight starts meanwhile (the party has to cover for it).
///
/// **How it acts** is the bots' own way: its feet by held movement keys (Keys, the seek helper's turn-and-forward)
/// run by the player controller every tick, never a spline or a teleport; its abilities by one press of the seat's own
/// action space a decision, through the same ApplySeatAction a learned seat's press goes through. Which press is the
/// sim's (StageScenario::DecideStandIn: today the "fight" baseline's choice from the seat's own row; the entity
/// actions, I1, are the seam there).
///
/// Pure -- no core types, its own random numbers from its seed -- so the draws and the behaviour switches are tested on
/// their own (StandInTest), and the scenario only supplies what the seat sees.
namespace Animus::Curriculum::StandIn
{
    enum class Role : uint8_t
    {
        Tank = 0,
        Healer,
        Damage,
        Count
    };
    constexpr std::array<char const*, uint32_t(Role::Count)> ROLE_NAMES = { "tank", "healer", "damage" };

    enum class Pace : uint8_t
    {
        Fast = 0,
        Slow,
    };

    enum class Quirk : uint8_t
    {
        PullEarly = 0,
        Wander,
        Rest,
        Lag,
        Afk,
        Count
    };
    constexpr uint32_t QUIRKS = uint32_t(Quirk::Count);
    constexpr std::array<char const*, QUIRKS> QUIRK_NAMES = { "pull_early", "wander", "rest", "lag", "afk" };

    /// What it is doing this decision: its plain play (Normal: leading or following, fighting when there is a fight),
    /// or one of its quirks.
    enum class Mode : uint8_t
    {
        Normal = 0,
        PullEarly,
        Wander,
        Rest,
        Lag,
        Afk,
    };

    [[nodiscard]] constexpr Mode ModeOf(Quirk quirk) { return Mode(uint8_t(quirk) + 1); }

    /// How often the stand-in plays, and how its styles are drawn (CurriculumTuning StandIn.*). Everything is off by
    /// default (Share 0): a stage turns it on.
    struct Tuning
    {
        /// Percent of a party or raid arena's training episodes with a stand-in in one of its seats. Evaluations have
        /// one only when the learner asks for the "with the human stand-in" arm (MODE_FLAG_STAND_IN), in every episode.
        int32_t Share = 0;
        int32_t LeadChance = 50;            // percent of styles that lead (only where the party has no owner)
        int32_t TankChance = 34;            // percent of styles that want to tank ...
        int32_t HealerChance = 33;          // ... or heal; the rest deal damage
        int32_t SlowChance = 50;            // percent of styles at the slow pace

        // Each quirk: the percent of styles that have it, how often it fires (per minute while its moment lasts), and
        // how long it lasts once it has.
        int32_t PullEarlyChance = 30;
        float PullEarlyPerMinute = 0.5f;
        uint32_t PullEarlyMinMs = 8000;
        uint32_t PullEarlyMaxMs = 20000;
        int32_t WanderChance = 40;
        float WanderPerMinute = 1.0f;
        uint32_t WanderMinMs = 6000;
        uint32_t WanderMaxMs = 15000;
        int32_t RestChance = 50;
        float RestPerMinute = 0.5f;
        uint32_t RestMinMs = 8000;
        uint32_t RestMaxMs = 20000;
        int32_t LagChance = 40;
        float LagPerMinute = 1.0f;
        uint32_t LagMinMs = 5000;
        uint32_t LagMaxMs = 15000;
        int32_t AfkChance = 25;
        float AfkPerMinute = 0.3f;
        uint32_t AfkMinMs = 10000;
        uint32_t AfkMaxMs = 30000;

        float RestBelow = 0.4f;             // a resting style always stops when its health or mana is under this
        float FollowYards = 6.0f;           // a follower keeps this close to the leader
        float LagYards = 25.0f;             // ... and this close while it lags
        float WanderMinYards = 10.0f;       // a wander's spot, from where it stands
        float WanderMaxYards = 25.0f;
        float MeleeYards = 3.0f;            // how close a tank or a melee build fights (and pulls) from
        float RangedYards = 25.0f;          // how far a ranged build fights and pulls from
        float HealerYards = 20.0f;          // a healer stands this far back from the fight
        uint32_t FastWaitMs = 1500;         // a fast pace's breather after a fight
        uint32_t SlowWaitMs = 8000;         // a slow pace's
        uint32_t FastReactMs = 250;         // a fast pace starts fighting this long after a fight starts
        uint32_t SlowReactMs = 1500;        // a slow pace's

        [[nodiscard]] int32_t Chance(Quirk quirk) const
        {
            switch (quirk)
            {
                case Quirk::PullEarly: return PullEarlyChance;
                case Quirk::Wander: return WanderChance;
                case Quirk::Rest: return RestChance;
                case Quirk::Lag: return LagChance;
                default: return AfkChance;
            }
        }

        [[nodiscard]] float PerMinute(Quirk quirk) const
        {
            switch (quirk)
            {
                case Quirk::PullEarly: return PullEarlyPerMinute;
                case Quirk::Wander: return WanderPerMinute;
                case Quirk::Rest: return RestPerMinute;
                case Quirk::Lag: return LagPerMinute;
                default: return AfkPerMinute;
            }
        }

        [[nodiscard]] std::pair<uint32_t, uint32_t> Lasts(Quirk quirk) const
        {
            switch (quirk)
            {
                case Quirk::PullEarly: return { PullEarlyMinMs, PullEarlyMaxMs };
                case Quirk::Wander: return { WanderMinMs, WanderMaxMs };
                case Quirk::Rest: return { RestMinMs, RestMaxMs };
                case Quirk::Lag: return { LagMinMs, LagMaxMs };
                default: return { AfkMinMs, AfkMaxMs };
            }
        }
    };

    /// Its own random numbers (splitmix64): the same seed plays the same person, whatever else the world drew.
    class Rng
    {
    public:
        explicit Rng(uint64_t seed = 0) : _state(seed) { }

        uint64_t Next()
        {
            uint64_t z = (_state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        /// Uniform in [0, 1).
        float Unit() { return float(Next() >> 40) / float(1ull << 24); }
        /// True `percent` percent of the time.
        bool Chance(int32_t percent) { return float(percent) > Unit() * 100.0f; }
        /// Uniform in [low, high].
        uint32_t Between(uint32_t low, uint32_t high)
        {
            return high <= low ? low : low + uint32_t(Next() % (uint64_t(high - low) + 1));
        }
        float Between(float low, float high) { return low + (high - low) * Unit(); }

    private:
        uint64_t _state;
    };

    /// An evaluation episode's stand-in seed from its seed index: the same index meets the same person in every
    /// evaluation, whichever env plays it.
    [[nodiscard]] inline uint64_t EvaluationSeed(uint32_t seedIndex)
    {
        Rng mix(0x5374616E64496EULL ^ (uint64_t(seedIndex) << 17));
        return mix.Next();
    }

    struct Style
    {
        uint64_t Seed = 0;
        bool Leads = false;
        Role Wanted = Role::Damage;         // what it would like to play; RoleFor says what it can
        Pace Speed = Pace::Fast;
        std::array<bool, QUIRKS> Quirks{};

        [[nodiscard]] bool Has(Quirk quirk) const { return Quirks[uint32_t(quirk)]; }
        [[nodiscard]] uint32_t WaitMs(Tuning const& tuning) const
        {
            return Speed == Pace::Slow ? tuning.SlowWaitMs : tuning.FastWaitMs;
        }
        [[nodiscard]] uint32_t ReactMs(Tuning const& tuning) const
        {
            return Speed == Pace::Slow ? tuning.SlowReactMs : tuning.FastReactMs;
        }
    };

    /// The style seed `seed` plays. `canLead`: the party has no owner, so the stand-in may be its leader.
    [[nodiscard]] inline Style Draw(uint64_t seed, Tuning const& tuning, bool canLead)
    {
        Rng rng(seed);
        Style style;
        style.Seed = seed;
        bool const leads = rng.Chance(tuning.LeadChance);
        style.Leads = canLead && leads;
        float const role = rng.Unit() * 100.0f;
        style.Wanted = role < float(tuning.TankChance) ? Role::Tank
            : role < float(tuning.TankChance + tuning.HealerChance) ? Role::Healer : Role::Damage;
        style.Speed = rng.Chance(tuning.SlowChance) ? Pace::Slow : Pace::Fast;
        for (uint32_t quirk = 0; quirk < QUIRKS; ++quirk)
            style.Quirks[quirk] = rng.Chance(tuning.Chance(Quirk(quirk)));
        return style;
    }

    /// The role it plays: the one it wants when its build can do it, else damage (which every build can deal).
    [[nodiscard]] constexpr Role RoleFor(Role wanted, bool canTank, bool canHeal)
    {
        if (wanted == Role::Tank && canTank)
            return Role::Tank;
        if (wanted == Role::Healer && canHeal)
            return Role::Healer;
        return Role::Damage;
    }

    /// What the seat sees this decision: what a person at the keyboard knows from the screen and the party frames.
    struct Situation
    {
        uint32_t NowMs = 0;
        uint32_t DecisionMs = 250;
        bool Alive = true;
        bool InCombat = false;              // it is in a fight itself
        bool PartyInCombat = false;         // someone in the party is
        bool HasTarget = false;             // an enemy to go to: the next pull, or what the party fights
        float TargetYards = 0.0f;
        bool HasLeader = false;             // a follower's leader (alive, in the world)
        float LeaderYards = 0.0f;
        float Health = 1.0f;                // shares of its maximum
        float Mana = 1.0f;                  // 1 for a build without mana
        bool Ranged = false;                // its build fights at range
    };

    /// Where it goes and whether it presses an ability this decision.
    struct Intent
    {
        enum class Goal : uint8_t
        {
            Hold,                           // let go of the keys (and face the target when fighting)
            Target,                         // to its target, stopping StopYards from it
            Leader,                         // to the leader, stopping StopYards from it
            Spot,                           // to the wander's spot (Behaviour::SpotAngle / SpotYards)
        };

        Mode Doing = Mode::Normal;
        Goal Go = Goal::Hold;
        float StopYards = 0.0f;
        bool Fight = false;                 // press an ability at its target
        bool Walk = false;
    };

    /// One episode of the stand-in: the style's plain play and its quirks, decision by decision.
    class Behaviour
    {
    public:
        Behaviour() = default;
        Behaviour(Style const& style, Role role) : _style(style), _role(role), _rng(style.Seed ^ 0xA5A5A5A5ull) { }

        [[nodiscard]] Style const& GetStyle() const { return _style; }
        [[nodiscard]] Role GetRole() const { return _role; }
        [[nodiscard]] Mode Current() const { return _mode; }
        /// How often each quirk fired this episode, and its ms spent in each mode (the episode info's).
        [[nodiscard]] uint32_t Fired(Quirk quirk) const { return _fired[uint32_t(quirk)]; }
        [[nodiscard]] uint32_t MsIn(Mode mode) const { return _msIn[uint32_t(mode)]; }
        /// A wander's spot: the bearing (radians, world frame) and distance from where the wander began.
        [[nodiscard]] float SpotAngle() const { return _spotAngle; }
        [[nodiscard]] float SpotYards() const { return _spotYards; }
        /// A new mode began this decision (the scenario takes the wander's origin then).
        [[nodiscard]] bool Started() const { return _started; }

        Intent Decide(Situation const& seen, Tuning const& tuning)
        {
            _started = false;
            bool const fighting = seen.InCombat || seen.PartyInCombat;
            if (fighting && !_fighting)
                _combatSinceMs = seen.NowMs;
            if (!fighting && _fighting)
                _peaceSinceMs = seen.NowMs;
            _fighting = fighting;

            if (!seen.Alive)
            {
                _mode = Mode::Normal;
                return {};
            }

            if (_mode != Mode::Normal && Over(seen, fighting))
                _mode = Mode::Normal;
            if (_mode == Mode::Normal)
                Roll(seen, fighting, tuning);

            _msIn[uint32_t(_mode)] += seen.DecisionMs;
            return IntentOf(seen, fighting, tuning);
        }

    private:
        [[nodiscard]] bool Over(Situation const& seen, bool fighting) const
        {
            if (seen.NowMs >= _untilMs)
                return true;
            switch (_mode)
            {
                case Mode::PullEarly: return seen.InCombat || !seen.HasTarget;     // it pulled, or nothing is left
                case Mode::Wander:
                case Mode::Rest: return fighting;                                   // a fight brings it back
                case Mode::Lag: return seen.InCombat || !seen.HasLeader;
                default: return false;                                              // AFK: away is away
            }
        }

        /// Whether `quirk` may fire now.
        [[nodiscard]] bool Moment(Quirk quirk, Situation const& seen, bool fighting) const
        {
            if (fighting)
                return false;
            switch (quirk)
            {
                case Quirk::PullEarly: return seen.HasTarget;
                case Quirk::Lag: return !_style.Leads && seen.HasLeader;
                default: return true;
            }
        }

        void Roll(Situation const& seen, bool fighting, Tuning const& tuning)
        {
            for (uint32_t index = 0; index < QUIRKS; ++index)
            {
                Quirk const quirk = Quirk(index);
                if (!_style.Has(quirk) || !Moment(quirk, seen, fighting))
                    continue;
                // A resting style always stops when it runs low; otherwise each quirk fires at its rate.
                bool const low = quirk == Quirk::Rest
                    && (seen.Health < tuning.RestBelow || seen.Mana < tuning.RestBelow);
                float const rate = std::max(0.0f, tuning.PerMinute(quirk));
                float const chance = 1.0f - std::exp(-rate * float(seen.DecisionMs) / 60000.0f);
                if (!low && !(_rng.Unit() < chance))
                    continue;
                auto const [shortest, longest] = tuning.Lasts(quirk);
                _mode = ModeOf(quirk);
                _untilMs = seen.NowMs + _rng.Between(std::min(shortest, longest), std::max(shortest, longest));
                ++_fired[index];
                _started = true;
                if (quirk == Quirk::Wander)
                {
                    _spotAngle = _rng.Between(0.0f, 6.2831853f);
                    _spotYards = _rng.Between(tuning.WanderMinYards, std::max(tuning.WanderMinYards,
                        tuning.WanderMaxYards));
                }
                return;
            }
        }

        [[nodiscard]] float FightYards(Situation const& seen, Tuning const& tuning) const
        {
            if (_role == Role::Healer)
                return tuning.HealerYards;
            if (_role == Role::Tank || !seen.Ranged)
                return tuning.MeleeYards;
            return tuning.RangedYards;
        }

        [[nodiscard]] Intent IntentOf(Situation const& seen, bool fighting, Tuning const& tuning) const
        {
            Intent intent;
            intent.Doing = _mode;
            switch (_mode)
            {
                case Mode::Afk:
                case Mode::Rest:
                    return intent;
                case Mode::Wander:
                    intent.Go = Intent::Goal::Spot;
                    intent.StopYards = 2.0f;
                    intent.Walk = true;
                    return intent;
                case Mode::Lag:
                    intent.Go = Intent::Goal::Leader;
                    intent.StopYards = tuning.LagYards;
                    return intent;
                case Mode::PullEarly:
                {
                    // It goes in to pull: a ranged build pulls from range, anything else walks up to it.
                    float const pull = seen.Ranged && _role != Role::Tank ? tuning.RangedYards : tuning.MeleeYards;
                    intent.Go = Intent::Goal::Target;
                    intent.StopYards = pull;
                    intent.Fight = seen.TargetYards <= pull + 1.0f;
                    return intent;
                }
                default:
                    break;
            }

            if (fighting)
            {
                // A moment to react, then into the fight at its role's distance.
                if (seen.NowMs - std::min(seen.NowMs, _combatSinceMs) < _style.ReactMs(tuning) || !seen.HasTarget)
                    return intent;
                intent.Go = Intent::Goal::Target;
                intent.StopYards = FightYards(seen, tuning);
                intent.Fight = true;
                return intent;
            }

            // Between fights: a breather at its pace, then on.
            if (seen.NowMs - std::min(seen.NowMs, _peaceSinceMs) < _style.WaitMs(tuning))
                return intent;
            if (_style.Leads)
            {
                if (!seen.HasTarget)
                    return intent;
                float const pull = seen.Ranged && _role != Role::Tank ? tuning.RangedYards : tuning.MeleeYards;
                intent.Go = Intent::Goal::Target;
                intent.StopYards = pull;
                intent.Fight = seen.TargetYards <= pull + 1.0f;
                return intent;
            }
            if (seen.HasLeader)
            {
                intent.Go = Intent::Goal::Leader;
                intent.StopYards = tuning.FollowYards;
            }
            return intent;
        }

        Style _style;
        Role _role = Role::Damage;
        Rng _rng;
        Mode _mode = Mode::Normal;
        uint32_t _untilMs = 0;
        bool _fighting = false;
        bool _started = false;
        uint32_t _combatSinceMs = 0;
        uint32_t _peaceSinceMs = 0;
        float _spotAngle = 0.0f;
        float _spotYards = 0.0f;
        std::array<uint32_t, QUIRKS> _fired{};
        std::array<uint32_t, 6> _msIn{};
    };

    /// The keys it holds this decision: the seek helper's turn-and-forward toward (goalX, goalY) until within `stop`
    /// yards when `move`; once there (or not moving), a turn toward (faceX, faceY) when `face`, so it is looking at
    /// what it fights. Nothing but held keys: the player controller moves the body under them.
    [[nodiscard]] inline Movement::ControlState Keys(Movement::BodyState const& body, bool move, float goalX,
        float goalY, float stop, bool face, float faceX, float faceY, bool walk)
    {
        Movement::ControlState control;
        if (move)
        {
            Movement::SeekTuning tuning;
            tuning.ArriveYards = std::max(0.5f, stop);
            control = Movement::Seek(body, goalX, goalY, tuning);
        }
        if (control.Forward == 0 && face)
        {
            float const dx = faceX - body.X;
            float const dy = faceY - body.Y;
            if (dx * dx + dy * dy > 0.01f)
            {
                float const error = std::remainder(std::atan2(dy, dx) - body.Yaw, 6.2831853f);
                control.TurnRate = std::fabs(error) < 0.1f ? 0.0f : std::clamp(error / 0.25f, -6.2831853f,
                    6.2831853f);
            }
        }
        control.Walk = walk && control.Forward != 0;
        return control;
    }
}

#endif
