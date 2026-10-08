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

#include "WorldCreatures.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"

namespace
{
    std::unordered_set<uint32> QueryIds(char const* sql)
    {
        std::unordered_set<uint32> ids;
        if (QueryResult result = WorldDatabase.Query(sql))
        {
            do
            {
                ids.insert(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        return ids;
    }
}

std::unordered_set<uint32> const& Animus::Curriculum::WorldCreatures::SpawnedIds()
{
    static std::unordered_set<uint32> const ids = QueryIds("SELECT DISTINCT id FROM creature");
    return ids;
}

std::unordered_set<uint32> const& Animus::Curriculum::WorldCreatures::WaypointWalkerIds()
{
    static std::unordered_set<uint32> const ids = QueryIds("SELECT DISTINCT c.id FROM creature c "
        "LEFT JOIN creature_addon a ON a.guid = c.guid WHERE c.MovementType = 2 OR IFNULL(a.path_id, 0) <> 0 "
        "UNION SELECT entry FROM creature_template_addon WHERE path_id <> 0");
    return ids;
}
