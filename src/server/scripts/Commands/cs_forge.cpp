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
#include "FieldGrids.h"
#include "LayeredField.h"
#include "MapWorldQuery.h"
#include "MapVisionWorld.h"
#include "FrameImage.h"
#include "VisionCaster.h"
#include "Capture.h"
#include "GameTime.h"
#include "MovementHandlerScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Replay.h"
#include "UnitBody.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include "InstanceBosses.h"
#include "StageDefinition.h"
#include "World.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include "FieldRoute.h"
#include "FloorScan.h"
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

    /// `forge controller record`: a Playtest player's movement packets, as the server took them (OnPlayerMove: after
    /// ReadMovementInfo's flag rules, before relocation), kept in the human-capture format (FORMAT.md Move and
    /// Speeds) until `record stop` writes them. One player at a time.
    struct MoveRecorder
    {
        std::atomic<bool> On{ false };
        std::mutex Lock;
        ObjectGuid Player;
        std::string Path;
        uint64 OpenedMs = 0;
        Animus::Movement::Capture::Recording Recording;
        Animus::Movement::Speeds LastSpeeds;
        bool HasSpeeds = false;
    };

    MoveRecorder& Recorder()
    {
        static MoveRecorder recorder;
        return recorder;
    }

    /// The hook the recorder listens on; costs one atomic load a movement packet while nothing is recorded (bots'
    /// reports reach it too, through ClientMovement::Apply).
    class ForgeMoveRecorderScript : public MovementHandlerScript
    {
    public:
        ForgeMoveRecorderScript() : MovementHandlerScript("ForgeMoveRecorderScript", { MOVEMENTHOOK_ON_PLAYER_MOVE })
        {
        }

        void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode) override
        {
            MoveRecorder& recorder = Recorder();
            if (!recorder.On.load(std::memory_order_relaxed) || !player)
                return;
            std::lock_guard guard(recorder.Lock);
            if (player->GetGUID() != recorder.Player)
                return;
            namespace Cap = Animus::Movement::Capture;
            uint64 const ms = uint64(GameTime::GetGameTimeMS().count());
            uint64 const id = player->GetGUID().GetCounter();
            // The unit's speeds whenever they change (mount, form, snare), as the capture's Speeds record does.
            Animus::Movement::Speeds const speeds = Animus::Movement::SpeedsOf(player);
            if (!recorder.HasSpeeds || std::memcmp(&speeds, &recorder.LastSpeeds, sizeof(speeds)) != 0)
            {
                recorder.Recording.Speeds.push_back(Cap::FromSpeeds(speeds, ms, id));
                recorder.LastSpeeds = speeds;
                recorder.HasSpeeds = true;
            }
            Cap::MoveRecord record;
            record.Ms = ms;
            record.Player = id;
            record.ClientMs = movementInfo.time;
            record.Opcode = uint16(opcode);
            record.Flags = movementInfo.GetMovementFlags();
            record.Flags2 = movementInfo.GetExtraMovementFlags();
            record.X = movementInfo.pos.GetPositionX();
            record.Y = movementInfo.pos.GetPositionY();
            record.Z = movementInfo.pos.GetPositionZ();
            record.O = movementInfo.pos.GetOrientation();
            record.Pitch = movementInfo.pitch;
            record.FallMs = movementInfo.fallTime;
            record.JumpZSpeed = movementInfo.jump.zspeed;
            record.JumpSin = movementInfo.jump.sinAngle;
            record.JumpCos = movementInfo.jump.cosAngle;
            record.JumpXYSpeed = movementInfo.jump.xyspeed;
            record.Map = player->GetMapId();
            recorder.Recording.Moves.push_back(record);
        }
    };

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
            static ChatCommandTable controllerCommandTable =
            {
                { "probe",  HandleControllerProbe,  SEC_ADMINISTRATOR, Console::Yes },
                { "record", HandleControllerRecord, SEC_ADMINISTRATOR, Console::Yes },
                { "replay", HandleControllerReplay, SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable cameraCommandTable =
            {
                { "snapshot", HandleCameraSnapshot, SEC_ADMINISTRATOR, Console::Yes },
            };

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
                { "floorscan", HandleFloorScan, SEC_ADMINISTRATOR, Console::Yes },
                { "fieldroute", HandleFieldRoute, SEC_ADMINISTRATOR, Console::Yes },
                { "fieldstage", HandleFieldStage, SEC_ADMINISTRATOR, Console::Yes },
                { "fieldworld", HandleFieldWorld, SEC_ADMINISTRATOR, Console::Yes },
                { "controller", controllerCommandTable },
                { "camera",    cameraCommandTable },
                { "tasks",     HandleTasks,     SEC_ADMINISTRATOR, Console::Yes },
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
            table.AddRow({ "forge controller record <player> <file> | stop", "record a Playtest player's movement packets (the human-capture format's Move and Speeds) until stopped, then write them" });
            table.AddRow({ "forge controller replay <file> [player]", "replay a recording (or a realm capture's move file) through the player controller: drift at 1/2/5/10 s, jumps, steps and slopes, and each client constant against what the recording measured (idle only)" });
            table.AddRow({ "forge controller probe <map> <x> <y> <z> [facing]", "the player controller's view of the world at a point (MapWorldQuery): the floor, its slope, the liquid, the free run along eight headings at the knee and the chest, the ceiling, and whether it is inside the terrain" });
            table.AddRow({ "forge camera snapshot <map> <x> <y> <z> <yaw> [pitch] [zoom] [file]", "render one frame of "
                "the vision block's camera from feet at (x, y, z), facing yaw degrees, pitch degrees (+ up) and zoom "
                "yd (default the AnimusForge.Vision.* ones), on the base map with no units and no objective: writes "
                "<file>-depth.pgm, <file>-kind.ppm and <file>-height.pgm (default file camera-snapshot) and prints the "
                "rays and the wall time, split into tree casts, WMO liquids, terrain cells and units; no range: a ray "
                "leaving the grids created round the feet is sky (idle only)" });
            table.AddRow({ "forge fieldstage <scenario> [rebake]", "bake the layered fields the dungeon wings' routes "
                "read for this scenario to AnimusForge.Probe.Dir: every grid of its maps' navmeshes and their "
                "neighbours (kept if already baked, unless rebake)" });
            table.AddRow({ "forge fieldworld <all|map id> [rebake]", "bake the layered fields of every grid of a map's "
                "navmesh, or of every map, for a realm's companions (mod-animus): into <Probe.Dir>/world, taking the "
                "stage fields already baked, and never read by the forge itself" });
            table.AddRow({ "forge tasks", "every map's update task since the last `forge tasks`: how many ran, "
                "their mean and longest time, and the envs on the map, slowest first" });
            table.AddRow({ "forge route <map> <x> <y> <z> <x> <y> <z>", "plan a way between two points and print "
                "it: corners, length against the straight line, and whether it arrives" });
            table.AddRow({ "forge talents <class> [spec] [points] [plan]",
                "print a build the curriculum would give that class (plan: standard, noisy, random)" });
            table.AddRow({ "forge bench [scenario]", "time the sim and the learner at every AnimusForge.Bench.* "
                "thread and env count" });
            table.AddRow({ "forge bench apply", "write the fastest settings from the last benchmark into the "
                "configs" });
            table.AddRow({ "forge bench auto [scenario]", "benchmark, then write the fastest settings into the configs "
                "and use them at once (what AnimusForge.Bench.AutoTune runs on a machine with no benchmark of its "
                "CPU)" });
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

        /// `forge floorscan <map> <x1> <y1> <x2> <y2> <z> [step] [file]`: the player controller's floor against the
        /// navmesh's walkable surface over a box, every `step` yards (FloorScan): where the route planner walks and
        /// the controller has no floor (HOLE), they disagree on the height (MISMATCH), the controller's floor is too
        /// steep (STEEP), or the controller has a floor the mesh does not walk (NONAV). Each cell's height comes
        /// from the navmesh within FloorScan::NAV_REACH_Z of `z` (ramps and stairs included), else `z`; the
        /// controller's floor is read from a step above it. A summary, the HOLE and MISMATCH cells, an ASCII map
        /// (a yard a character, the worst cell shown), and with `file` a CSV of every cell. On the base map (no
        /// instance needed), only while the forge is idle.
        static bool HandleFloorScan(ChatHandler* handler, uint32 mapId, float x1, float y1, float x2, float y2, float z,
            Optional<float> step, Optional<std::string> file)
        {
            namespace Mv = Animus::Movement;
            namespace Scan = Animus::Curriculum::FloorScan;
            if (!sAnimusForge->IsIdle())
            {
                handler->SendSysMessage("forge floorscan runs only while the forge is idle (it creates grids)");
                return true;
            }
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }
            float const cell = std::clamp(step.value_or(0.5f), 0.1f, 10.0f);
            float const left = std::min(x1, x2), right = std::max(x1, x2);
            float const bottom = std::min(y1, y2), top = std::max(y1, y2);
            uint32 const columns = uint32(std::floor((right - left) / cell)) + 1;
            uint32 const rows = uint32(std::floor((top - bottom) / cell)) + 1;
            if (uint64(columns) * rows > 1000000)
            {
                handler->PSendSysMessage("{} x {} cells is too many: a larger step, or a smaller box", columns, rows);
                return true;
            }
            CreateGrids(map, left, bottom, right, top);

            Mv::MapWorldQuery const world(map, PHASEMASK_NORMAL);
            Animus::Curriculum::RoutePlanner& planner = Animus::Curriculum::RoutePlanner::Instance();
            std::ofstream csv;
            if (file)
            {
                csv.open(*file);
                if (!csv)
                {
                    handler->PSendSysMessage("Could not write {}", *file);
                    return true;
                }
                csv << "x,y,navmesh_z,floor_z,normal_z,class\n";
            }
            std::array<uint32, 6> counts{};
            std::vector<std::string> listed;
            uint32 const mapColumns = uint32(std::ceil(right - left)) + 1;
            uint32 const mapRows = uint32(std::ceil(top - bottom)) + 1;
            std::vector<Scan::Cell> worst(std::size_t(mapColumns) * mapRows, Scan::Cell::Unwalkable);
            for (uint32 row = 0; row < rows; ++row)
                for (uint32 column = 0; column < columns; ++column)
                {
                    float const x = left + float(column) * cell;
                    float const y = bottom + float(row) * cell;
                    float navZ = 0.0f;
                    bool const nav = planner.SurfaceAt(map, x, y, z, Scan::NAV_REACH_Z, navZ);
                    float const reference = nav ? navZ : z;
                    float const floor = world.FloorBelow(x, y, reference + Mv::STEP_UP, 2.0f * Mv::STEP_UP);
                    bool const hasFloor = floor > Mv::INVALID_FLOOR + 1.0f;
                    float const normal = hasFloor ? world.FloorNormalZ(x, y, floor) : 0.0f;
                    Scan::Cell const verdict = Scan::Classify(nav, navZ, hasFloor, floor, normal);
                    ++counts[std::size_t(verdict)];
                    if ((verdict == Scan::Cell::Hole || verdict == Scan::Cell::Mismatch) && listed.size() < 40)
                        listed.push_back(Acore::StringFormat("  {} at ({:.1f}, {:.1f}): navmesh {:.2f}, floor {}",
                            Scan::Name(verdict), x, y, navZ, hasFloor ? Acore::StringFormat("{:.2f}", floor) : "none"));
                    std::size_t const at = std::size_t(std::min<uint32>(uint32(y - bottom), mapRows - 1)) * mapColumns
                        + std::min<uint32>(uint32(x - left), mapColumns - 1);
                    if (Scan::Severity(verdict) > Scan::Severity(worst[at]))
                        worst[at] = verdict;
                    if (csv)
                        csv << Acore::StringFormat("{:.2f},{:.2f},{},{},{},{}\n", x, y,
                            nav ? Acore::StringFormat("{:.2f}", navZ) : "", hasFloor ? Acore::StringFormat("{:.2f}",
                            floor) : "", hasFloor ? Acore::StringFormat("{:.3f}", normal) : "", Scan::Name(verdict));
                }

            handler->PSendSysMessage("floorscan map {} x {:.1f}..{:.1f} y {:.1f}..{:.1f} at z {:.1f} (navmesh within "
                "{:.0f} yd), {} cells of {:.2f} yd:", mapId, left, right, bottom, top, z, Scan::NAV_REACH_Z,
                columns * rows, cell);
            for (Scan::Cell verdict : { Scan::Cell::Ok, Scan::Cell::Hole, Scan::Cell::Mismatch, Scan::Cell::Steep,
                     Scan::Cell::NoNav, Scan::Cell::Unwalkable })
                handler->PSendSysMessage("  {:<10} {}", Scan::Name(verdict), counts[std::size_t(verdict)]);
            if (!listed.empty())
            {
                handler->SendSysMessage("HOLE and MISMATCH cells (the first 40):");
                for (std::string const& line : listed)
                    handler->SendSysMessage(line);
            }
            handler->PSendSysMessage("A yard a character, the worst cell shown ('.' OK, 'H' HOLE, 'M' MISMATCH, 'S' "
                "STEEP, 'n' NONAV, ' ' UNWALKABLE); x from {:.0f} rightwards, y from {:.0f} at the top down:", left,
                top);
            for (uint32 row = mapRows; row-- > 0;)
            {
                std::string line = Acore::StringFormat("{:>7.1f} |", bottom + float(row));
                for (uint32 column = 0; column < mapColumns; ++column)
                    line += Scan::Glyph(worst[std::size_t(row) * mapColumns + column]);
                handler->SendSysMessage(line + "|");
            }
            if (file)
                handler->PSendSysMessage("Every cell in {}", *file);
            return true;
        }

        /// A map's grid as the core names it (GridCoord, the mmtile file name) from a field's grid index
        /// (FieldGrids::GridIndex): the core counts grids from +x/+y down, the fields from 0 up.
        static GridCoord CoreGrid(int32 gridX, int32 gridY)
        {
            return GridCoord(uint32(std::clamp(int32(CENTER_GRID_ID) - 1 - gridX, 0, int32(MAX_NUMBER_OF_GRIDS) - 1)),
                uint32(std::clamp(int32(CENTER_GRID_ID) - 1 - gridY, 0, int32(MAX_NUMBER_OF_GRIDS) - 1)));
        }

        /// `forge camera snapshot <map> <x> <y> <z> <yaw> [pitch] [zoom] [file]`: one frame of the vision block's
        /// camera (camera-vision), from feet at (x, y, z) facing `yaw` degrees, the camera at `pitch` degrees (+ up)
        /// and `zoom` yards (the AnimusForge.Vision.* ones by default), with a default body, no units and no
        /// objective. Writes <file>-depth.pgm (the distance channel, near dark), <file>-kind.ppm (a colour a kind,
        /// the objective white) and <file>-height.pgm (the height channel, mid-grey at the feet), and prints the rays
        /// cast and the wall time, split into the tree casts, the WMO liquids, the terrain cells and the units. On
        /// the base map, with the grid holding the feet and its eight neighbours created (terrain, collision and
        /// navmesh tiles only, no creatures: CreateGrids) -- a ray leaving them is sky -- only while idle.
        static bool HandleCameraSnapshot(ChatHandler* handler, uint32 mapId, float x, float y, float z, float yaw,
            Optional<float> pitch, Optional<float> zoom, Optional<std::string> file)
        {
            namespace Vi = Animus::Vision;
            if (!sAnimusForge->IsIdle())
            {
                handler->SendSysMessage("forge camera snapshot runs only while the forge is idle (it creates grids)");
                return true;
            }
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }

            Vi::Settings const settings = Vi::Current();
            Vi::CameraState camera;
            camera.Pitch = std::clamp(pitch.value_or(settings.Pitch), -80.0f, 80.0f) * Vi::DEGREES;
            camera.Zoom = std::clamp(zoom.value_or(settings.Zoom), 0.0f, 50.0f);
            // The grid of the feet and its neighbours, as a seat's map has them around it: a ray has no range, and
            // reads sky where it leaves the grids created.
            CreateGrids(map, x - Vi::GRID_SIZE, y - Vi::GRID_SIZE, x + Vi::GRID_SIZE, y + Vi::GRID_SIZE);

            Vi::Pose pose;
            pose.X = x;
            pose.Y = y;
            pose.Z = z;
            pose.Yaw = yaw * Vi::DEGREES;
            pose.BodyHeight = Animus::Movement::Body().Height;
            Vi::MapVisionWorld const world(map, PHASEMASK_NORMAL);
            // The frame as the block sends it: the image's bytes and the scalars.
            std::vector<uint8> image(Vi::ImageBytes(settings));
            std::array<float, Vi::SCALARS> scalars{};

            // Once as the block renders (no clock inside), once with the breakdown's clocks.
            auto const start = std::chrono::steady_clock::now();
            uint32 const rays = Vi::Render(settings, pose, camera, world, {}, nullptr, image.data(), scalars.data());
            double const wallUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()
                - start).count();
            Vi::Breakdown breakdown;
            Vi::Render(settings, pose, camera, world, {}, nullptr, image.data(), scalars.data(), &breakdown);

            std::string const base = file.value_or("camera-snapshot");
            uint32 const width = settings.Width;
            uint32 const height = settings.Height;
            uint32 const pixels = width * height;
            // Decoded as the learner decodes the bytes.
            std::vector<float> decoded(std::size_t(pixels) * Vi::CHANNELS);
            for (uint32 pixel = 0; pixel < pixels; ++pixel)
                Vi::DecodePixel(&image[std::size_t(pixel) * Vi::BYTES_PER_PIXEL], &decoded[std::size_t(pixel)
                    * Vi::CHANNELS]);
            auto const channel = [&](uint32 pixel, uint32 index) { return decoded[std::size_t(pixel) * Vi::CHANNELS
                + index]; };
            auto const byte = [](float value) { return char(uint8(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f)); };
            std::array<uint32, Vi::KINDS> kinds{};
            std::string depth, kind, rise;
            for (uint32 pixel = 0; pixel < pixels; ++pixel)
            {
                depth += byte(channel(pixel, Vi::CHANNEL_DISTANCE));
                rise += byte((channel(pixel, Vi::CHANNEL_HEIGHT) + 1.0f) / 2.0f);
                uint32 const what = std::min<uint32>(uint32(channel(pixel, Vi::CHANNEL_KIND)), Vi::KINDS - 1);
                ++kinds[what];
                bool const objective = channel(pixel, Vi::CHANNEL_OBJECTIVE) > 0.5f;
                for (uint32 c = 0; c < 3; ++c)
                    kind += char(objective ? 255 : Vi::KIND_COLOURS[what][c]);
            }
            auto const write = [&](std::string const& path, char const* magic, std::string const& data)
            {
                std::ofstream out(path, std::ios::binary);
                out << magic << "\n" << width << " " << height << "\n255\n" << data;
                if (!out)
                    handler->PSendSysMessage("Could not write {}", path);
            };
            write(base + "-depth.pgm", "P5", depth);
            write(base + "-kind.ppm", "P6", kind);
            write(base + "-height.pgm", "P5", rise);
            // The four panels in one image, as the training audit saves them (AnimusForge.Vision.AuditInterval).
            {
                std::ofstream png(base + ".png", std::ios::binary);
                png << Vi::FramePng(settings, image.data(), std::max<uint32>(1, 256 / std::max<uint32>(1,
                    settings.Width)));
                if (!png)
                    handler->PSendSysMessage("Could not write {}", base + ".png");
            }

            handler->PSendSysMessage("camera snapshot map {} feet ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} pitch {:.1f} "
                "zoom {:.1f}: {} x {} pixels, {:.0f} x {:.0f} degrees, no range", mapId, x, y, z, yaw,
                camera.Pitch / Vi::DEGREES, camera.Zoom, width, height, settings.FovH, settings.FovV);
            handler->PSendSysMessage("  {} rays in {:.0f} us ({:.2f} us a ray)", rays, wallUs,
                rays ? wallUs / double(rays) : 0.0);
            handler->PSendSysMessage("  with the breakdown's clocks: trees {:.0f} us ({} casts), WMO liquids {:.0f} us "
                "({} casts), terrain {:.0f} us ({} grids, {} cells), units {:.0f} us ({} tests)",
                double(breakdown.TreeNs) / 1e3, breakdown.TreeCasts, double(breakdown.LiquidNs) / 1e3,
                breakdown.LiquidCasts, double(breakdown.TerrainNs) / 1e3, breakdown.TerrainTiles,
                breakdown.TerrainCells, double(breakdown.UnitNs) / 1e3, breakdown.UnitTests);
            handler->PSendSysMessage("  boom {:.2f} yd, pivot above floor {:.2f} (/10), underwater {}, airborne {}",
                scalars[Vi::SCALAR_BOOM] * Vi::ZOOM_SCALE, scalars[Vi::SCALAR_PIVOT_HEIGHT],
                scalars[Vi::SCALAR_UNDERWATER] > 0.5f ? "yes" : "no",
                scalars[Vi::SCALAR_AIRBORNE] > 0.5f ? "yes" : "no");
            std::string histogram;
            for (uint32 what = 0; what < Vi::KINDS; ++what)
                if (kinds[what])
                    histogram += Acore::StringFormat("{}{} {}", histogram.empty() ? "" : ", ", Vi::KIND_NAMES[what],
                        kinds[what]);
            handler->PSendSysMessage("  pixels by kind: {}", histogram);
            handler->PSendSysMessage("  wrote {0}-depth.pgm, {0}-kind.ppm, {0}-height.pgm and {0}.png", base);
            return true;
        }

        /// `forge controller probe <map> <x> <y> <z> [facing]`: what the player controller's live world query reads at
        /// a point
        /// (player-controller C2). On the world thread, with nothing training: it creates the grids it reads.
        static bool HandleControllerProbe(ChatHandler* handler, uint32 mapId, float x, float y, float z,
            Optional<float> facing)
        {
            namespace Mv = Animus::Movement;
            // It creates grids on the world thread; the console is shared with live training.
            if (!sAnimusForge->IsIdle())
            {
                handler->SendSysMessage("the controller probe runs only while the forge is idle");
                return true;
            }
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }
            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                    map->EnsureGridCreated(CoreGrid(Animus::Curriculum::FieldGrids::GridIndex(x) + dx,
                        Animus::Curriculum::FieldGrids::GridIndex(y) + dy));

            Mv::MapWorldQuery const world(map, PHASEMASK_NORMAL);
            Mv::Body const body;
            float const floor = world.FloorBelow(x, y, z + Mv::STEP_UP, 2.0f * Mv::STEP_UP);
            bool const hasFloor = floor > Mv::INVALID_FLOOR + 1.0f;
            float const feet = hasFloor ? floor : z;
            Mv::Liquid const liquid = world.LiquidAt(x, y, feet);
            handler->PSendSysMessage("controller at map {} ({:.2f}, {:.2f}, {:.2f}): STEP_UP {:.2f} (provisional, C6)",
                mapId, x, y, z, Mv::STEP_UP);
            if (hasFloor)
                handler->PSendSysMessage("  floor {:.2f} ({:+.2f} from z), normal.z {:.3f} ({}walkable at 50 deg)",
                    floor, floor - z, world.FloorNormalZ(x, y, floor),
                    world.FloorNormalZ(x, y, floor) >= Mv::WALKABLE_NORMAL_Z ? "" : "not ");
            else
                handler->PSendSysMessage("  no floor within a step of z");
            if (liquid.Present)
                handler->PSendSysMessage("  liquid level {:.2f} ({:.2f} deep over the feet){}", liquid.Level,
                    liquid.Level - feet, liquid.Deadly ? ", deadly" : "");
            handler->PSendSysMessage("  ceiling {:.1f} yd above the head; feet {}inside the terrain",
                world.Ceiling(x, y, feet + body.Height, 50.0f), world.InTerrain(x, y, feet) ? "" : "not ");
            float const yaw = facing.value_or(0.0f);
            for (int32 heading = 0; heading < 8; ++heading)
            {
                float const angle = yaw + float(heading) * float(M_PI) / 4.0f;
                float const reach = 20.0f;
                float const share = world.Sweep(x, y, feet, x + reach * std::cos(angle), y + reach * std::sin(angle),
                    feet, body);
                handler->PSendSysMessage("  heading {:>3.0f} deg: free {:.1f} of {:.0f} yd", angle * 180.0f / M_PI,
                    share * reach, reach);
            }
            return true;
        }

        /// `forge controller record <player> <file>` starts recording that player's movement packets (a Playtest
        /// client's); `forge controller record stop` writes them (player-controller C6). One player at a time.
        static bool HandleControllerRecord(ChatHandler* handler, std::string name, Optional<std::string> file)
        {
            namespace Cap = Animus::Movement::Capture;
            MoveRecorder& recorder = Recorder();
            if (name == "stop")
            {
                Cap::Recording recording;
                std::string path;
                uint64 opened = 0;
                {
                    std::lock_guard guard(recorder.Lock);
                    if (!recorder.On.exchange(false))
                    {
                        handler->SendSysMessage("nothing is being recorded");
                        return true;
                    }
                    recording = std::move(recorder.Recording);
                    path = recorder.Path;
                    opened = recorder.OpenedMs;
                    recorder.Recording = Cap::Recording();
                }
                std::string error;
                if (!Cap::WriteFile(path, recording, opened, error))
                {
                    handler->PSendSysMessage("could not write the recording: {}", error);
                    return true;
                }
                handler->PSendSysMessage("wrote {} movement packets and {} speed records to {}; read it with `forge "
                    "controller replay {}`", recording.Moves.size(), recording.Speeds.size(), path, path);
                return true;
            }
            if (!file || file->empty())
            {
                handler->SendSysMessage("usage: forge controller record <player> <file> | forge controller record "
                    "stop");
                return true;
            }
            Player* player = ObjectAccessor::FindPlayerByName(name, false);
            if (!player || !player->GetSession() || player->GetSession()->IsSimSession())
            {
                handler->PSendSysMessage("no player named {} with a client is online", name);
                return true;
            }
            std::lock_guard guard(recorder.Lock);
            recorder.Player = player->GetGUID();
            recorder.Path = *file;
            recorder.OpenedMs = uint64(GameTime::GetGameTimeMS().count());
            recorder.Recording = Cap::Recording();
            recorder.HasSpeeds = false;
            recorder.On = true;
            handler->PSendSysMessage("recording {}'s movement packets; `forge controller record stop` writes them to "
                "{}", name, *file);
            return true;
        }

        /// `forge controller replay <file> [player]`: the recording's packets fed through the player controller over
        /// the live world (MapWorldQuery on the recording's map), and the report (Animus::Movement::Replay). Idle
        /// only: it creates the grids it reads. A realm capture's move file holds many players: the one with the most
        /// packets, unless one is named by its capture id.
        static bool HandleControllerReplay(ChatHandler* handler, std::string file, Optional<uint64> player)
        {
            namespace Mv = Animus::Movement;
            namespace Cap = Animus::Movement::Capture;
            if (!sAnimusForge->IsIdle())
            {
                handler->SendSysMessage("the controller replay runs only while the forge is idle");
                return true;
            }
            Cap::Recording recording;
            std::string error;
            if (!Cap::ReadFile(file, recording, error))
            {
                handler->PSendSysMessage("could not read {}: {}", file, error);
                return true;
            }
            uint64 chosen = player.value_or(0);
            if (!player)
            {
                std::map<uint64, uint32> counts;
                for (Cap::MoveRecord const& move : recording.Moves)
                    ++counts[move.Player];
                for (auto const& [id, count] : counts)
                    if (!chosen || count > counts[chosen])
                        chosen = id;
            }
            std::vector<Cap::MoveRecord> moves;
            for (Cap::MoveRecord const& move : recording.Moves)
                if (move.Player == chosen && move.Source == 0)
                    moves.push_back(move);
            if (moves.empty())
            {
                handler->PSendSysMessage("{} has no client packets of player {}", file, chosen);
                return true;
            }
            // One map: the first packet's (a recording that crosses maps is replayed on the first).
            uint32 const mapId = moves.front().Map;
            std::erase_if(moves, [mapId](Cap::MoveRecord const& move) { return move.Map != mapId; });
            std::stable_sort(moves.begin(), moves.end(), [](Cap::MoveRecord const& a, Cap::MoveRecord const& b)
            {
                return a.Ms < b.Ms;
            });
            std::vector<Cap::SpeedsRecord> speeds;
            for (Cap::SpeedsRecord const& s : recording.Speeds)
                if (s.Player == chosen)
                    speeds.push_back(s);

            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }
            std::set<std::pair<uint32, uint32>> grids;
            for (Cap::MoveRecord const& move : moves)
                for (int32 dx = -1; dx <= 1; ++dx)
                    for (int32 dy = -1; dy <= 1; ++dy)
                        grids.emplace(Animus::Curriculum::FieldGrids::GridIndex(move.X) + dx,
                            Animus::Curriculum::FieldGrids::GridIndex(move.Y) + dy);
            for (auto const& [gx, gy] : grids)
                map->EnsureGridCreated(CoreGrid(gx, gy));

            Mv::MapWorldQuery const world(map, PHASEMASK_NORMAL);
            Mv::Body const shape;   // a player's own cylinder is not in the capture: the controller's default
            Mv::Replay::Report const report = Mv::Replay::Run(moves, speeds, shape, world);
            handler->PSendSysMessage("player {} on map {}, {} speed records:", chosen, mapId, speeds.size());
            std::string const text = report.Text();
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t const end = text.find('\n', start);
                handler->SendSysMessage(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return true;
        }

        /// `forge tasks`: the map update's tasks by map since the last call, slowest mean first -- where the map
        /// update's wall comes from when one map's task is much longer than the rest.
        static bool HandleTasks(ChatHandler* handler)
        {
            struct Row
            {
                uint32 MapId;
                uint32 InstanceId;
                uint32 Envs;
                Map::TaskTotals Totals;
            };
            std::vector<Row> rows;
            sMapMgr->DoForAllMaps([&rows](Map* map)
            {
                Map::TaskTotals const totals = map->TakeTaskTotals();
                if (totals.Count)
                    rows.push_back({ map->GetId(), map->GetInstanceId(), sAnimusForge->EnvsOnMap(*map), totals });
            });
            std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b)
            {
                return a.Totals.SumNs * b.Totals.Count > b.Totals.SumNs * a.Totals.Count;
            });

            using Align = AnimusForge::TextTable::Align;
            AnimusForge::TextTable table({ { "Map" }, { "Instance", Align::Right }, { "Envs", Align::Right },
                { "Tasks", Align::Right }, { "Mean ms", Align::Right }, { "Longest ms", Align::Right },
                { "Sum ms", Align::Right } });
            for (Row const& row : rows)
                table.AddRow({ std::to_string(row.MapId), std::to_string(row.InstanceId), std::to_string(row.Envs),
                    std::to_string(row.Totals.Count),
                    Acore::StringFormat("{:.3f}", double(row.Totals.SumNs) / double(row.Totals.Count) / 1e6),
                    Acore::StringFormat("{:.3f}", double(row.Totals.MaxNs) / 1e6),
                    Acore::StringFormat("{:.1f}", double(row.Totals.SumNs) / 1e6) });
            handler->PSendSysMessage("Map update tasks since the last `forge tasks` ({} maps):", rows.size());
            table.Write(Reply(handler), "  ");
            return true;
        }

        /// `forge fieldstage <scenario> [rebake]`: the layered fields the dungeon wings' routes read for this
        /// scenario (FieldRoute) -- every grid of its maps' navmeshes, continents whole, and their neighbours, since a
        /// route near a grid's edge crosses it. A grid with no floor in it (past a dungeon's edge) is
        /// written too, a few bytes, so that a missing file means a grid not baked and never "no floor here".
        /// Static geometry only; a grid takes a fraction of a second.
        static bool HandleFieldStage(ChatHandler* handler, std::string scenario, Optional<std::string> mode)
        {
            namespace Bake = Animus::Curriculum::FieldGrids;
            namespace Field = Animus::Curriculum::LayeredField;
            Animus::Curriculum::StageDefinition const* stage = Animus::Curriculum::FindStage(scenario);
            if (!stage)
            {
                handler->PSendSysMessage("No such scenario: {}", scenario);
                return true;
            }
            bool const rebake = mode && *mode == "rebake";

            std::set<Bake::GridRef> grids;
            for (Bake::GridRef const& grid : Bake::StageGrids(*stage, true))
                for (int32 dx = -1; dx <= 1; ++dx)
                    for (int32 dy = -1; dy <= 1; ++dy)
                        // Within the world's 64 x 64 grids: a map at its edge has no neighbour past it.
                        if (std::abs(2 * (grid.X + dx) + 1) < MAX_NUMBER_OF_GRIDS
                            && std::abs(2 * (grid.Y + dy) + 1) < MAX_NUMBER_OF_GRIDS)
                            grids.insert(Bake::GridRef{ grid.MapId, grid.X + dx, grid.Y + dy });
            handler->PSendSysMessage("{}: {} grids (every grid of its maps' navmeshes and their neighbours), into {}",
                scenario,
                grids.size(), Field::Store::Dir());
            uint32 baked = 0;
            uint32 kept = 0;
            uint32 empty = 0;
            uint32 failed = 0;
            std::size_t bytes = 0;
            auto const started = std::chrono::steady_clock::now();
            for (Bake::GridRef const& grid : grids)
            {
                Map* map = sMapMgr->CreateBaseMap(grid.MapId);
                if (!map)
                    continue;

                std::string const path = Field::Store::FileFor(grid.MapId, grid.X, grid.Y);
                std::error_code error;
                if (!rebake && std::filesystem::exists(path, error))
                {
                    ++kept;
                    continue;
                }

                // Terrain and collision only: no objects are spawned.
                for (int32 dx = -1; dx <= 1; ++dx)
                    for (int32 dy = -1; dy <= 1; ++dy)
                        map->EnsureGridCreated(CoreGrid(grid.X + dx, grid.Y + dy));

                float const centreX = (float(grid.X) + 0.5f) * SIZE_OF_GRIDS;
                float const centreY = (float(grid.Y) + 0.5f) * SIZE_OF_GRIDS;
                Field::Grid const field = Field::Bake(map, centreX, centreY, Field::STANDARD_CELL);
                empty += field.Intervals.empty() ? 1 : 0;
                if (Field::Write(field, path))
                {
                    ++baked;
                    bytes += std::filesystem::file_size(path, error);
                }
                else
                {
                    ++failed;
                    handler->PSendSysMessage("  could not write {}", path);
                }
            }

            double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            handler->PSendSysMessage("{}: {} grids baked ({:.1f} MB, {} of them with no floor), {} already there, "
                "{} failed, in {:.0f} s", scenario, baked, double(bytes) / (1024.0 * 1024.0), empty, kept, failed,
                seconds);
            return true;
        }

        /// `forge fieldworld <all|map id> [rebake]`: the layered fields a realm's companions read wherever their
        /// players take them (mod-animus, Animus.Probe.Dir) -- every grid of a map's navmesh and its neighbours, or
        /// of every map's. Into <Probe.Dir>/world, not beside the stage fields: the forge ships those to every cluster
        /// machine, and the world's are gigabytes no training reads. A grid already baked for a stage is copied, not
        /// rebaked.
        static bool HandleFieldWorld(ChatHandler* handler, std::string which, Optional<std::string> mode)
        {
            namespace Bake = Animus::Curriculum::FieldGrids;
            namespace Field = Animus::Curriculum::LayeredField;
            bool const rebake = mode && *mode == "rebake";
            Optional<uint32> onlyMap;
            if (which != "all")
            {
                onlyMap = Acore::StringTo<uint32>(which);
                if (!onlyMap)
                {
                    handler->PSendSysMessage("forge fieldworld <all|map id> [rebake]");
                    return true;
                }
            }

            // Every mmtile, MMMXXYY.mmtile: XX and YY count down from +x/+y where the grid indices count up from 0.
            std::set<Bake::GridRef> grids;
            std::error_code error;
            for (auto const& file : std::filesystem::directory_iterator(sWorld->GetDataPath() + "mmaps", error))
            {
                std::string const name = file.path().filename().string();
                if (name.size() != 14 || file.path().extension() != ".mmtile")
                    continue;
                uint32 const mapId = uint32(std::atoi(name.substr(0, 3).c_str()));
                if (onlyMap && mapId != *onlyMap)
                    continue;
                int32 const x = int32(CENTER_GRID_ID) - 1 - std::atoi(name.substr(3, 2).c_str());
                int32 const y = int32(CENTER_GRID_ID) - 1 - std::atoi(name.substr(5, 2).c_str());
                for (int32 dx = -1; dx <= 1; ++dx)
                    for (int32 dy = -1; dy <= 1; ++dy)
                        if (std::abs(2 * (x + dx) + 1) < MAX_NUMBER_OF_GRIDS
                            && std::abs(2 * (y + dy) + 1) < MAX_NUMBER_OF_GRIDS)
                            grids.insert(Bake::GridRef{ mapId, x + dx, y + dy });
            }

            std::filesystem::path const dir = std::filesystem::path(Field::Store::Dir()) / "world";
            std::filesystem::create_directories(dir, error);
            handler->PSendSysMessage("fieldworld {}: {} grids, into {}", which, grids.size(), dir.string());
            uint32 baked = 0;
            uint32 copied = 0;
            uint32 kept = 0;
            uint32 failed = 0;
            uint32 lastMap = UINT32_MAX;
            Map* map = nullptr;
            auto const started = std::chrono::steady_clock::now();
            for (Bake::GridRef const& grid : grids)
            {
                std::filesystem::path const staged = Field::Store::FileFor(grid.MapId, grid.X, grid.Y);
                std::filesystem::path const path = dir / staged.filename();
                if (!rebake && std::filesystem::exists(path, error))
                {
                    ++kept;
                    continue;
                }
                if (!rebake && std::filesystem::exists(staged, error))
                {
                    std::filesystem::copy_file(staged, path, std::filesystem::copy_options::overwrite_existing, error);
                    ++(error ? failed : copied);
                    continue;
                }

                if (grid.MapId != lastMap)
                {
                    lastMap = grid.MapId;
                    map = sMapMgr->CreateBaseMap(grid.MapId);
                }
                if (!map)
                {
                    ++failed;
                    continue;
                }
                // Terrain and collision only: no objects are spawned.
                for (int32 dx = -1; dx <= 1; ++dx)
                    for (int32 dy = -1; dy <= 1; ++dy)
                        map->EnsureGridCreated(CoreGrid(grid.X + dx, grid.Y + dy));
                float const centreX = (float(grid.X) + 0.5f) * SIZE_OF_GRIDS;
                float const centreY = (float(grid.Y) + 0.5f) * SIZE_OF_GRIDS;
                if (Field::Write(Field::Bake(map, centreX, centreY, Field::STANDARD_CELL), path.string()))
                    ++baked;
                else
                {
                    ++failed;
                    handler->PSendSysMessage("  could not write {}", path.string());
                }
            }

            double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            handler->PSendSysMessage("fieldworld {}: {} grids baked, {} copied from the stage fields, {} already "
                "there, {} failed, in {:.0f} s", which, baked, copied, kept, failed, seconds);
            return true;
        }

        /// Plan a route between two points, with no seat, policy or run.
        ///
        /// The bench for the route planner, and the answer to a question an evaluation cannot ask: when an
        /// episode fails, was there ever a way? PathGenerator says PATHFIND_NORMAL when it has not pathfound at
        /// all, so "reachable" has meant less than it reads. This plans with the planner's own query -- a large
        /// node pool, no 74-point cap -- and says plainly whether the way arrives or stops short.
        /// Plan a way over the layered field (FieldRoute), the ground the seats walk by, between two points: what a
        /// dungeon's route is built from. Its fields must be baked (`forge fieldstage`).
        static bool HandleFieldRoute(ChatHandler* handler, uint32 mapId, float fromX, float fromY, float fromZ,
            float toX, float toY, float toZ)
        {
            Position const from(fromX, fromY, fromZ, 0.0f);
            Position const to(toX, toY, toZ, 0.0f);
            handler->SendSysMessage(Animus::Curriculum::FieldRoute::Report(mapId, from, to));
            return true;
        }

        /// Every grid of `map` over the box, with a grid's margin: terrain, collision and navmesh tiles only
        /// (EnsureGridCreated). Not LoadGrid: that loads the grid's creatures too, which an instanced map's base map --
        /// one with no instance, as the console reads it -- cannot hold (`forge route 34 ...` crashed the sim).
        static void CreateGrids(Map* map, float x1, float y1, float x2, float y2)
        {
            namespace Scan = Animus::Curriculum::FloorScan;
            auto const [gx1, gx2] = Scan::GridSpan(x1, x2, 1.0f);
            auto const [gy1, gy2] = Scan::GridSpan(y1, y2, 1.0f);
            for (int32 gx = gx1; gx <= gx2; ++gx)
                for (int32 gy = gy1; gy <= gy2; ++gy)
                    map->EnsureGridCreated(CoreGrid(gx, gy));
        }

        static bool HandleRoute(ChatHandler* handler, uint32 mapId, float fromX, float fromY, float fromZ,
            float toX, float toY, float toZ)
        {
            if (!sAnimusForge->IsIdle())
            {
                handler->SendSysMessage("forge route runs only while the forge is idle (it creates grids)");
                return true;
            }
            Map* map = sMapMgr->CreateBaseMap(mapId);
            if (!map)
            {
                handler->PSendSysMessage("No such map: {}", mapId);
                return true;
            }

            // Every grid between the ends: their navmesh tiles, and the way's.
            CreateGrids(map, fromX, fromY, toX, toY);

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
        static bool HandleBench(ChatHandler* handler, Optional<std::string> argument, Optional<std::string> scenario)
        {
            std::string const value = argument.value_or("");
            if (value == "apply")
                return sAnimusForge->CommandBenchApply(Reply(handler));
            // `forge bench auto [scenario]`: the same, and the winner applied (configs and this sim) when done.
            if (value == "auto")
                return sAnimusForge->CommandBench(scenario.value_or(""), Reply(handler), true);

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
    new ForgeMoveRecorderScript();
}
