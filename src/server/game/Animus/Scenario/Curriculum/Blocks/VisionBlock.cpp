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
#include "FreeLook.h"
#include "MapVisionWorld.h"
#include "Player.h"
#include "SeatView.h"
#include "UnitBody.h"
#include "VisionCaster.h"
#include "VisionCost.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
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
    // Revision 4: the learner's patch at this canonical size (a 16-wide grid), and the sizes frames are actually
    // cast at before they are scaled up to it, [width, height] each.
    image["patch"] = Vi::Patch(settings);
    boost::json::array sizes;
    for (Vi::Resolution const& size : settings.RenderSizes)
        sizes.push_back(boost::json::array{ size.Width, size.Height });
    image["render_sizes"] = std::move(sizes);
    block["image"] = std::move(image);

    // The look head (free look): three categoricals, in ACT's order. Every choice is always allowed: no mask.
    boost::json::object look;
    boost::json::array heads;
    boost::json::array names;
    for (uint32 head = 0; head < Vi::FreeLook::HEADS; ++head)
    {
        heads.push_back(Vi::FreeLook::HEAD_SIZES[head]);
        names.push_back(boost::json::string(Vi::FreeLook::HEAD_NAMES[head]));
    }
    look["heads"] = std::move(heads);
    look["names"] = std::move(names);
    block["look"] = std::move(look);

    // What the image means beyond its shape: the camera it was rendered with (informational).
    boost::json::object camera;
    camera["mode"] = "free";
    boost::json::array yawRates;
    for (float rate : Vi::FreeLook::YAW_RATES_DEG)
        yawRates.push_back(double(rate));
    camera["yaw_rates"] = std::move(yawRates);
    boost::json::array pitchRates;
    for (float rate : Vi::FreeLook::PITCH_RATES_DEG)
        pitchRates.push_back(double(rate));
    camera["pitch_rates"] = std::move(pitchRates);
    boost::json::array zoomLevels;
    for (float level : Vi::FreeLook::ZOOM_LEVELS)
        zoomLevels.push_back(double(level));
    camera["zoom_levels"] = std::move(zoomLevels);
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
    // The camera first, whatever else this seat can render: the held rates over the decision (none at the episode's
    // first observation), follow mode easing it back while the forward key is held (R2: the key, not the body).
    // The state is advanced from a const block, which is safe only because Observe runs once per seat per decision:
    // a reset clears Observed, and ObserveAfterJoin is refused for vision stages. Anything that observes a seat twice
    // in a decision would advance its camera twice.
    if (Vi::FreeLook::State* look = view.Look)
        Vi::FreeLook::Advance(*look, float(view.DecisionMs) / 1000.0f,
            view.Controls && view.Controls->Held.Forward > 0);

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

    // The seat's own camera; a view without one sees in follow mode, behind the facing at the conf's pitch and zoom.
    Vi::CameraState camera;
    if (view.Look)
        camera = Vi::FreeLook::CameraOf(*view.Look);
    else
    {
        camera.Pitch = settings.Pitch * Vi::DEGREES;
        camera.Zoom = settings.Zoom;
    }

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
