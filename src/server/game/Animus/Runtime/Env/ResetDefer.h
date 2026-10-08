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

#ifndef ANIMUS_RESET_DEFER_H
#define ANIMUS_RESET_DEFER_H

#include <functional>

/// The world-thread half of an episode reset that runs on a map thread (EnvPool, AnimusForge.ResetOnMapThreads).
///
/// A reset that keeps its env on its own map builds the new episode on the thread updating that map, right after the
/// map's tick: the characters, their gear and spells, the encounter, the route. What it may not do there is touch
/// what every map shares and nothing locks -- the character cache, the social and instance-bind managers, a
/// character's logout (groups, LFG, script hooks) and the pool's own indexes, which other maps' damage hooks read
/// mid-tick. Those calls go through Run: on a map-thread reset they are queued, in order, and the world thread runs
/// them in FinishCollect, after the maps have joined and before any of them ticks again; anywhere else they run at
/// once, as they always did.
namespace Animus::ResetDefer
{
    /// While one is alive, Run on this thread queues instead of running.
    class Scope
    {
    public:
        Scope();
        ~Scope();
        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;

    private:
        bool _outer;
    };

    /// Whether this thread is inside a map-thread reset.
    [[nodiscard]] bool Active();

    /// `work` now, or queued for Flush when Active.
    void Run(std::function<void()> work);

    /// World thread, after the maps have joined: everything queued, in the order it was queued.
    void Flush();
}

#endif
