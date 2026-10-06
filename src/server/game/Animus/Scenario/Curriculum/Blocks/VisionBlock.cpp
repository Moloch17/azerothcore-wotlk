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

#include "VisionBlock.h"
#include "Camera.h"
#include "MapVisionWorld.h"
#include "Player.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "VisionCaster.h"
#include "VisionCost.h"
#include <boost/json/object.hpp>
#include <chrono>
#include <vector>

namespace Vi = Animus::Vision;

Animus::Curriculum::BlockSize Animus::Curriculum::VisionBlock::Size(Layout const& /*layout*/) const
{
    return BlockSize{ Vi::ObsCount(Vi::Current()), 0 };
}

void Animus::Curriculum::VisionBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    Vi::Settings const& settings = Vi::Current();
    boost::json::object image;
    image["height"] = settings.Height;
    image["width"] = settings.Width;
    image["channels"] = Vi::CHANNELS;
    image["kinds"] = Vi::KINDS;
    image["kind_channel"] = Vi::KIND_CHANNEL;
    image["scalars"] = Vi::SCALARS;
    // Revision 3: the image is not in the float columns but its own byte section of the STEP, four bytes a pixel.
    image["transport"] = "bytes";
    image["bytes_per_pixel"] = Vi::BYTES_PER_PIXEL;
    block["image"] = std::move(image);

    // What the image means beyond its shape: the camera it was rendered with (informational).
    boost::json::object camera;
    camera["mode"] = "follow";
    camera["fov_h"] = double(settings.FovH);
    camera["fov_v"] = double(settings.FovV);
    // A ray has no range: the distance channel is log-scaled to a fixed reference, and Range is the units' radius.
    camera["distance_reference"] = double(Vi::DISTANCE_REFERENCE);
    camera["unit_range"] = double(settings.Range);
    camera["caster"] = "raycast";
    camera["zoom"] = double(settings.Zoom);
    camera["pitch"] = double(settings.Pitch);
    camera["yaw_offset"] = 0.0;
    block["camera"] = std::move(camera);
}

void Animus::Curriculum::VisionBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    Player* bot = view.Bot;
    Map* map = bot && bot->IsInWorld() ? bot->GetMap() : nullptr;
    if (!map)
        return;

    auto const start = std::chrono::steady_clock::now();
    Vi::Settings const& settings = Vi::Current();

    // The seat's own view of itself: the controller's body, as a client knows its own place (MoveBlock reads it so
    // too); the server's position where there is no body. Airborne is the body falling or flying (not swimming).
    Vi::Pose pose;
    if (Movement::BodyState const* body = view.Body)
    {
        pose.X = body->X;
        pose.Y = body->Y;
        pose.Z = body->Z;
        pose.Airborne = body->Kind == Movement::Mode::Falling || body->Kind == Movement::Mode::Flying;
    }
    else
    {
        pose.X = bot->GetPositionX();
        pose.Y = bot->GetPositionY();
        pose.Z = bot->GetPositionZ();
        pose.Airborne = !bot->IsInWater() && (bot->IsFalling() || bot->IsFlying());
    }
    pose.Yaw = view.Facing;
    pose.BodyHeight = Movement::ShapeOf(bot).Height;

    // Follow mode, fixed for this slice: behind the facing, the conf's pitch and zoom.
    Vi::CameraState camera;
    camera.YawOffset = 0.0f;
    camera.Pitch = settings.Pitch * Vi::DEGREES;
    camera.Zoom = settings.Zoom;

    Vi::MapVisionWorld const world(map, bot->GetPhaseMask());
    // The units within range of where the camera can be: the pivot, with the zoom added to the reach.
    thread_local std::vector<Vi::UnitShape> units;
    Vi::Vec3 const pivot{ pose.X, pose.Y, pose.Z + Vi::PIVOT_SHARE * pose.BodyHeight };
    Vi::GatherUnits(bot, pivot, settings.Range + camera.Zoom, units);

    Vi::Vec3 const objective{ view.Objective.GetPositionX(), view.Objective.GetPositionY(),
        view.Objective.GetPositionZ() };
    // The image into the seat's byte row (none: the scalars alone, and no pixel cast), the scalars into the columns.
    uint32 const rays = Vi::Render(settings, pose, camera, world, units, view.HasObjective ? &objective : nullptr,
        view.Image, obs);
    Vi::Cost::Add(uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
        - start).count()), rays);
}
