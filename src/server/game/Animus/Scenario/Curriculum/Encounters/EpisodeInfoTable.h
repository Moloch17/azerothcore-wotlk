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

#ifndef ANIMUS_LIB_CURRICULUM_EPISODE_INFO_TABLE_H
#define ANIMUS_LIB_CURRICULUM_EPISODE_INFO_TABLE_H

#include "Define.h"
#include <functional>
#include <string>
#include <vector>

namespace Animus
{
    struct Env;
}

namespace Animus::Curriculum
{
    /// The per-seat episode statistics a scenario reports, each column's name declared with the function that fills
    /// it, so the two can never drift apart. Columns come in the order they are added.
    class EpisodeInfoTable
    {
    public:
        using Getter = std::function<float(Env const& env, uint32 seat)>;

        void Add(std::string name, Getter getter)
        {
            _names.push_back(std::move(name));
            _getters.push_back(std::move(getter));
        }

        [[nodiscard]] bool Contains(std::string const& name) const
        {
            for (std::string const& existing : _names)
                if (existing == name)
                    return true;
            return false;
        }

        [[nodiscard]] uint32 Size() const { return uint32(_names.size()); }
        [[nodiscard]] std::vector<std::string> const& Names() const { return _names; }

        /// One seat's row: Size() values.
        void Write(Env const& env, uint32 seat, float* info) const
        {
            for (std::size_t i = 0; i < _getters.size(); ++i)
                info[i] = _getters[i](env, seat);
        }

    private:
        std::vector<std::string> _names;
        std::vector<Getter> _getters;
    };
}

#endif
