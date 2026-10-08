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

#include "ResetDefer.h"
#include <mutex>
#include <vector>

namespace
{
    thread_local bool t_active = false;
    std::mutex g_lock;
    std::vector<std::function<void()>> g_queued;
}

Animus::ResetDefer::Scope::Scope() : _outer(t_active)
{
    t_active = true;
}

Animus::ResetDefer::Scope::~Scope()
{
    t_active = _outer;
}

bool Animus::ResetDefer::Active()
{
    return t_active;
}

void Animus::ResetDefer::Run(std::function<void()> work)
{
    if (!t_active)
    {
        work();
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    g_queued.push_back(std::move(work));
}

void Animus::ResetDefer::Flush()
{
    std::vector<std::function<void()>> queued;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        queued.swap(g_queued);
    }
    for (std::function<void()>& work : queued)
        work();
}
