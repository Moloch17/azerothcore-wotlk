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

#include "AnimusForge.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Map.h"
#include "MapMgr.h"
#include "MoveBlock.h"
#include "RoutePlanner.h"
#include "Optional.h"
#include "StringConvert.h"
#include "TextTable.h"

using namespace Acore::ChatCommands;

namespace
{
    /// A level 80 character's talent points: what `forge talents` shows when given no count.
    constexpr uint32 TALENT_POINTS_AT_80 = 71;

    /// Scenario names separated by spaces or commas.
    std::vector<std::string> SplitNames(std::string_view text)
    {
        std::vector<std::string> names;
        std::string name;
        for (char c : text)
        {
            if (c == ' ' || c == ',' || c == '\t')
            {
                if (!name.empty())
                    names.push_back(std::move(name));
                name.clear();
            }
            else
                name += c;
        }

        if (!name.empty())
            names.push_back(std::move(name));

        return names;
    }

    AnimusForge::LineSink Reply(ChatHandler* handler)
    {
        return [handler](std::string const& line) { handler->SendSysMessage(line); };
    }

    class ForgeCommandScript : public CommandScript
    {
    public:
        ForgeCommandScript() : CommandScript("ForgeCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable forgeCommandTable =
            {
                { "help",      HandleHelp,      SEC_ADMINISTRATOR, Console::Yes },
                { "status",    HandleStatus,    SEC_ADMINISTRATOR, Console::Yes },
                { "scenarios", HandleScenarios, SEC_ADMINISTRATOR, Console::Yes },
                { "start",     HandleStart,     SEC_ADMINISTRATOR, Console::Yes },
                { "fast",      HandleFast,      SEC_ADMINISTRATOR, Console::Yes },
                { "resume",    HandleResume,    SEC_ADMINISTRATOR, Console::Yes },
                { "pause",     HandlePause,     SEC_ADMINISTRATOR, Console::Yes },
                { "cancel",    HandleCancel,    SEC_ADMINISTRATOR, Console::Yes },
                { "skip",      HandleSkip,      SEC_ADMINISTRATOR, Console::Yes },
                { "run",       HandleRun,       SEC_ADMINISTRATOR, Console::Yes },
                { "rays",      HandleRays,      SEC_ADMINISTRATOR, Console::Yes },
                { "route",     HandleRoute,     SEC_ADMINISTRATOR, Console::Yes },
                { "bench",     HandleBench,     SEC_ADMINISTRATOR, Console::Yes },
                { "talents",   HandleTalents,   SEC_ADMINISTRATOR, Console::Yes },
                { "export",    HandleExport,    SEC_ADMINISTRATOR, Console::Yes },
                { "clean",     HandleClean,     SEC_ADMINISTRATOR, Console::Yes },
                { "progress",  HandleProgress,  SEC_ADMINISTRATOR, Console::Yes },
                { "",          HandleHelp,      SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "forge", forgeCommandTable },
            };

            return commandTable;
        }

        static bool HandleHelp(ChatHandler* handler)
        {
            AnimusForge::TextTable table({ { "Command" }, { "What it does" } });
            table.AddRow({ "forge status", "what is running, progress, ETA and warnings (or the idle settings)" });
            table.AddRow({ "forge scenarios", "every scenario with its run: checkpoint, steps, best score" });
            table.AddRow({ "forge start [scenario ...]", "train these from scratch in order (default: "
                "AnimusForge.Queue)" });
            table.AddRow({ "forge fast [scenario ...]", "quick low-resolution test run of these (default: "
                "AnimusForge.Queue) in the fast output directory" });
            table.AddRow({ "forge resume [scenario ...]", "unpause; or continue the first from its latest.pt, then the "
                "rest (default: where the last plan stopped)" });
            table.AddRow({ "forge pause", "freeze the sim and the learner after the current decision" });
            table.AddRow({ "forge cancel", "stop the plan; the learner saves latest.pt first" });
            table.AddRow({ "forge skip", "end the current scenario and start the next one" });
            table.AddRow({ "forge run <scenario> <policy> [episodes]", "run a scripted or random policy, no learner" });
            table.AddRow({ "forge rays <map> <x> <y> <z> [facing]", "what the navmesh senses read standing there: "
                "reach, shore, water width, burning edge and clearance" });
            table.AddRow({ "forge route <map> <x> <y> <z> <x> <y> <z>", "plan a way between two points and print "
                "it: corners, length against the straight line, and whether it arrives" });
            table.AddRow({ "forge talents <class> [spec] [points] [plan]",
                "print a build the curriculum would give that class (plan: standard, noisy, random)" });
            table.AddRow({ "forge bench [scenario]", "time the sim and the learner at every AnimusForge.Bench.* "
                "thread and env count" });
            table.AddRow({ "forge bench apply", "write the fastest settings from the last benchmark into the "
                "configs" });
            table.AddRow({ "forge export [scenario] [best|latest]", "write the scenario's .amdl models to "
                "AnimusForge.ModelDir" });
            table.AddRow({ "forge clean archive", "delete runs/_archive/" });
            table.AddRow({ "forge clean scenario <scenario>", "delete runs/<scenario>/ (its checkpoints and logs)" });
            table.AddRow({ "forge clean exports", "delete the exported models" });
            table.AddRow({ "forge clean fast", "delete the fast test runs, layouts and models" });
            table.AddRow({ "forge clean logs", "delete the learner and export logs" });
            table.AddRow({ "forge clean all", "all of the above, every run included (idle only)" });
            table.AddRow({ "forge progress [seconds|off]", "show or set the periodic progress report interval" });

            handler->SendSysMessage("Animus Forge console commands:");
            table.Write(Reply(handler), "  ");
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            sAnimusForge->CommandStatus(Reply(handler));
            return true;
        }

        static bool HandleScenarios(ChatHandler* handler)
        {
            sAnimusForge->CommandScenarios(Reply(handler));
            return true;
        }

        static bool HandleStart(ChatHandler* handler, Tail scenarios)
        {
            return sAnimusForge->CommandStart(SplitNames(scenarios), Reply(handler));
        }

        static bool HandleFast(ChatHandler* handler, Tail scenarios)
        {
            return sAnimusForge->CommandFast(SplitNames(scenarios), Reply(handler));
        }

        static bool HandleResume(ChatHandler* handler, Tail scenarios)
        {
            return sAnimusForge->CommandResume(SplitNames(scenarios), Reply(handler));
        }

        static bool HandlePause(ChatHandler* handler)
        {
            return sAnimusForge->CommandPause(Reply(handler));
        }

        static bool HandleCancel(ChatHandler* handler)
        {
            return sAnimusForge->CommandCancel(Reply(handler));
        }

        static bool HandleSkip(ChatHandler* handler)
        {
            return sAnimusForge->CommandSkip(Reply(handler));
        }

        static bool HandleRun(ChatHandler* handler, std::string scenario, std::string policy, Optional<uint32> episodes)
        {
            return sAnimusForge->CommandRun(scenario, policy, episodes.value_or(0), Reply(handler));
        }

        /// Plan a route between two points, with no seat, policy or run.
        ///
        /// The bench for the route planner, and the answer to a question an evaluation cannot ask: when an
        /// episode fails, was there ever a way? PathGenerator says PATHFIND_NORMAL when it has not pathfound at
        /// all, so "reachable" has meant less than it reads. This plans with the planner's own query -- a large
        /// node pool, no 74-point cap -- and says plainly whether the way arrives or stops short.
        static bool HandleRoute(ChatHandler* handler, uint32 mapId, float fromX, float fromY, float fromZ,
            float toX, float toY, float toZ)
        {
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }

            // Both ends want their tiles in, and they are rarely the same tile.
            map->LoadGrid(fromX, fromY);
            map->LoadGrid(toX, toY);

            handler->PSendSysMessage("Route on map {} from ({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f}):",
                mapId, fromX, fromY, fromZ, toX, toY, toZ);

            Position const from(fromX, fromY, fromZ, 0.0f);
            Position const to(toX, toY, toZ, 0.0f);
            std::string const report = Animus::Curriculum::RoutePlanner::Instance().Report(map, from, to);

            std::string line;
            for (char c : report)
            {
                if (c == '\n')
                {
                    handler->SendSysMessage(line);
                    line.clear();
                }
                else
                    line += c;
            }

            if (!line.empty())
                handler->SendSysMessage(line);

            return true;
        }

        /// Read the movement block's navmesh senses at one point, without a seat, a policy or a run.
        ///
        /// Every one of those senses is a Detour query, and Detour's axes are {y, z, x} rather than the world's.
        /// A swizzle that is wrong is silent: the rays go somewhere else and return entirely plausible numbers
        /// about the wrong place. Training cannot catch it -- it shows up only as a policy that learns worse
        /// than it should, hours later, with nothing to point at. This puts the numbers next to geometry whose
        /// answer is already known: a wall at a measured distance, a corridor, a lake that has been swum.
        static bool HandleRays(ChatHandler* handler, uint32 mapId, float x, float y, float z,
            Optional<float> facing)
        {
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }

            // The navmesh tile comes in with the grid, so an unvisited corner of the world answers nothing until
            // it is asked for.
            map->LoadGrid(x, y);

            handler->PSendSysMessage("Rays at map {} ({:.2f}, {:.2f}, {:.2f}) facing {:.2f}:",
                mapId, x, y, z, facing.value_or(0.0f));

            std::string const report =
                Animus::Curriculum::MoveBlock::RayReport(map, x, y, z, facing.value_or(0.0f));

            std::string line;
            for (char c : report)
            {
                if (c == '\n')
                {
                    handler->SendSysMessage(line);
                    line.clear();
                }
                else
                    line += c;
            }

            if (!line.empty())
                handler->SendSysMessage(line);

            return true;
        }

        /// `forge talents <class> [spec] [points] [plan]` prints a build the curriculum would give that
        /// class: which talents, in which tree, at how many ranks.
        static bool HandleTalents(ChatHandler* handler, std::string playerClass, Optional<std::string> spec,
            Optional<uint32> points, Optional<std::string> plan)
        {
            return sAnimusForge->CommandTalents(playerClass, spec.value_or(""), points.value_or(TALENT_POINTS_AT_80),
                plan.value_or(""), Reply(handler));
        }

        /// `forge bench [scenario]` times the sim at every thread and env count in AnimusForge.Bench.*, then the
        /// fastest few with the learner. `forge bench apply` writes the winner into the configs.
        static bool HandleBench(ChatHandler* handler, Optional<std::string> argument)
        {
            std::string const value = argument.value_or("");
            if (value == "apply")
                return sAnimusForge->CommandBenchApply(Reply(handler));

            return sAnimusForge->CommandBench(value, Reply(handler));
        }

        static bool HandleExport(ChatHandler* handler, Optional<std::string> first, Optional<std::string> second)
        {
            // `forge export best` exports the current scenario's best.pt.
            std::string scenario = first.value_or("");
            std::string checkpoint = second.value_or("");
            if (checkpoint.empty() && (scenario == "best" || scenario == "latest"))
                std::swap(scenario, checkpoint);

            return sAnimusForge->CommandExport(scenario, checkpoint, Reply(handler));
        }

        static bool HandleClean(ChatHandler* handler, Optional<std::string> target, Optional<std::string> scenario)
        {
            return sAnimusForge->CommandClean(target.value_or(""), scenario.value_or(""), Reply(handler));
        }

        static bool HandleProgress(ChatHandler* handler, Optional<std::string> interval)
        {
            if (!interval)
            {
                sAnimusForge->CommandProgress(std::nullopt, Reply(handler));
                return true;
            }

            if (*interval == "off")
            {
                sAnimusForge->CommandProgress(0, Reply(handler));
                return true;
            }

            Optional<uint32> const seconds = Acore::StringTo<uint32>(*interval);
            if (!seconds)
            {
                handler->SendSysMessage("Usage: forge progress [seconds|off]");
                return false;
            }

            sAnimusForge->CommandProgress(*seconds, Reply(handler));
            return true;
        }
    };
}

void AddSC_forge_commandscript()
{
    new ForgeCommandScript();
}
