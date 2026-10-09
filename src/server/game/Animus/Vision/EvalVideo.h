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

#ifndef ANIMUS_VISION_EVAL_VIDEO_H
#define ANIMUS_VISION_EVAL_VIDEO_H

#include "Camera.h"
#include "Identity.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// **Evaluation videos** (dungeon-curriculum I0; AnimusForge.Vision.EvalVideos): a few of every evaluation's episodes
/// filmed as the bot saw them, one frame a decision, each frame the composite (CompositeRgb, the mini-map inset where
/// the stage has a map), written as an animated PNG with a JSON sidecar under runs/<scenario>/videos/<env steps>/.
///
/// The world thread only copies the bytes the learner was sent (the camera image and the map crop, ~47 KB a seat);
/// a worker thread composites, scales, deflates and writes, so training never waits on a video.
namespace Animus::Vision
{
    /// **The seeds an evaluation films**: `count` of the seed indexes [first, end), the same ones every evaluation
    /// with the same seeds (so successive checkpoints play the same episodes), whatever the episodes turn out to be.
    /// An evaluation's seed i plays (class, build) pair i mod `pairs` and rung i / `pairs` (StageScenario::DrawCasting,
    /// DifficultyLadder): pick k of `count` takes pair k * pairs / count of cycle k * cycles / count, so the picks
    /// fall on different classes (all different when pairs >= count) and spread over the rungs. Sorted, distinct, in
    /// range; fewer than `count` only when the range is smaller.
    [[nodiscard]] std::vector<uint32_t> EvalVideoSeeds(uint32_t first, uint32_t end, uint32_t count, uint32_t pairs);

    /// The episode info columns that say whether an episode did what its stage asks, in the order they are looked for.
    constexpr char const* EVAL_VIDEO_OUTCOMES[] = { "found", "arrived", "won", "cleared", "success", "survived" };

    /// "success" or "failure" by the first EVAL_VIDEO_OUTCOMES column the stage reports (over 0.5 is a success), else
    /// "ended"; "unfinished" without a row (the evaluation stopped first).
    [[nodiscard]] std::string EvalVideoOutcome(std::vector<std::string> const& names, float const* row);

    /// The column holding the episode's rung: "rung" or "tier", or a name ending "_rung" or "_tier" (not at_top_rung,
    /// a flag), else `difficulty`; -1 for none.
    [[nodiscard]] int32_t EvalVideoRungColumn(std::vector<std::string> const& names);

    /// One seat an evaluation episode could film: its agent (the learner plays it: the "human" stand-in's and a
    /// director's are not candidates), its place in the party (Scenario::FilmedRole: 1 tank, 2 healer, 3 damage, 0
    /// none) and its class.
    struct EvalVideoCandidate
    {
        uint32_t Agent = 0;
        uint32_t Role = 0;
        uint32_t Class = 0;
    };

    /// The role names a sidecar writes (EvalVideoEpisode::Role): "" for none.
    [[nodiscard]] char const* EvalVideoRoleName(uint32_t role);

    /// **Whom a party's episode films** (I0 for multi-seat envs): an evaluation's video `pick` (its seed's place among
    /// the evaluation's EvalVideoSeeds) films the place it comes to in turn -- tank, healer, damage, pick mod 3, the
    /// next one along where the party has nobody in that place -- and of the seats in that place the (pick / 3)-th in
    /// class order (then agent), so an evaluation's videos spread over the party's places and its classes. A seed's
    /// party is drawn from the seed, so the same seed films the same seat every evaluation. Candidates with no place
    /// at all (a solo stage, a party with none drawn) are taken in turn, class order then agent. 0 for none.
    [[nodiscard]] uint32_t EvalVideoAgent(uint32_t pick, std::vector<EvalVideoCandidate> candidates);

    /// Who an episode filmed: written into its sidecar. Taken when the recording starts (after the episode ends, the
    /// env's seat is already the next episode's character).
    struct EvalVideoEpisode
    {
        uint32_t Seed = 0;
        uint32_t Env = 0;
        uint32_t Agent = 0;
        std::string Layout;
        uint32_t Class = 0;
        uint32_t Race = 0;
        uint32_t Level = 0;
        uint32_t Role = 0;                  // its place in the party (EvalVideoCandidate::Role), 0 for none
        uint32_t RenderWidth = 0;           // the size the camera cast at this episode (RenderSizes), 0 for none known
        uint32_t RenderHeight = 0;
    };

    /// One evaluation's recording settings.
    struct EvalVideoOptions
    {
        std::filesystem::path Dir;          // runs/<scenario>/videos/<label>
        std::string Scenario;
        std::string Label;                  // the learner's env steps at the evaluation, or a stand-in
        Settings Camera;                    // the frames' size (the canonical image)
        uint32_t Scale = 4;                 // each image pixel drawn scale x scale
        uint32_t DecisionMs = 250;          // a frame's time: the video plays at the game's speed
        uint32_t ImageBytes = 0;            // per frame, the camera's bytes (ImageBytes(Camera))
        uint32_t MapBytes = 0;              // per frame, the map crop's (CROP_BYTES), 0 without a map block
        uint32_t MaxFrames = 0;             // frames kept per episode, 0 for every one
        std::vector<std::string> InfoNames; // the episode info columns, for Finish's row
    };

    /// Films the episodes of one evaluation at a time. Its calls are the world thread's; the work is a worker's.
    class EvalVideoRecorder
    {
    public:
        /// Unwritten frames held at once before new ones are dropped (and counted in the sidecar): the worker has
        /// fallen behind, and training must not wait for it.
        static constexpr std::size_t MAX_QUEUED_BYTES = std::size_t(512) << 20;

        EvalVideoRecorder() = default;
        ~EvalVideoRecorder();
        EvalVideoRecorder(EvalVideoRecorder const&) = delete;
        EvalVideoRecorder& operator=(EvalVideoRecorder const&) = delete;

        /// An evaluation starts: film `seeds` (EvalVideoSeeds) as envs play them. Ends the one before, if any.
        void Begin(EvalVideoOptions options, std::vector<uint32_t> seeds);
        /// The evaluation is over: recordings still open close as "unfinished", and once every video is written the
        /// directory gets its index.json and the log its "Eval videos: N recorded to <dir>" line.
        void End();

        [[nodiscard]] bool Active() const { return _evaluation != nullptr; }
        /// `seed` is one to film and no env has started it yet.
        [[nodiscard]] bool Wanted(uint32_t seed) const;
        /// `seed`'s place among the evaluation's seeds to film (EvalVideoAgent's pick); 0 for one not among them.
        [[nodiscard]] uint32_t PickOf(uint32_t seed) const;
        [[nodiscard]] bool Recording(uint32_t env) const { return _open.count(env) != 0; }
        /// The open recording's seed and seat (Recording(env) first).
        [[nodiscard]] uint32_t Seed(uint32_t env) const;
        [[nodiscard]] uint32_t Agent(uint32_t env) const;

        /// The episode starts being filmed: its env's frames go to it until Finish.
        void Start(EvalVideoEpisode episode);
        /// A decision's frame: the camera's ImageBytes and the map's MapBytes (null without a map), copied; and the
        /// entities the frame listed (EntityMark, null for none), which the worker draws over the picture: the image
        /// is the static world only, so who was in view is the marks'.
        void Frame(uint32_t env, uint8_t const* image, uint8_t const* map,
            std::vector<EntityMark> const* marks = nullptr);
        /// The episode ended with info row `info` (InfoNames' columns; null when it did not end): its video is written.
        void Finish(uint32_t env, float const* info);

        /// Wait for the worker to write everything queued (tests; shutdown).
        void Drain();

        /// For `forge status`: videos written since the forge started, the directory of the last evaluation indexed.
        [[nodiscard]] uint64_t Recorded() const { return _recorded.load(std::memory_order_relaxed); }
        [[nodiscard]] std::string LastDir() const;
        /// Frames dropped because the worker fell behind (MAX_QUEUED_BYTES), since the forge started.
        [[nodiscard]] uint64_t Dropped() const { return _droppedTotal; }

    private:
        struct Evaluation;
        struct Reel;

        struct Job
        {
            enum class Kind
            {
                Frame,
                Close,
                Index,
            };

            Kind What = Kind::Frame;
            std::shared_ptr<Evaluation> Of;
            std::shared_ptr<Reel> Film;
            std::vector<uint8_t> Bytes;         // Frame: the image, then the map
            std::vector<EntityMark> Marks;      // Frame: the entities listed, drawn over the picture
            std::vector<float> Info;            // Close: the info row, empty when the episode did not end
        };

        void Push(Job job);
        void Work();
        void WriteFrame(Job const& job);
        void WriteVideo(Job const& job);
        void WriteIndex(Evaluation& evaluation);

        // The world thread's.
        std::shared_ptr<Evaluation> _evaluation;
        std::vector<uint32_t> _wanted;
        std::unordered_map<uint32_t, std::shared_ptr<Reel>> _open;
        uint64_t _droppedTotal = 0;

        // Shared with the worker.
        mutable std::mutex _lock;
        std::condition_variable _wake;
        std::condition_variable _idle;
        std::deque<Job> _jobs;
        std::size_t _queuedBytes = 0;
        bool _busy = false;
        bool _stop = false;
        std::string _lastDir;
        std::atomic<uint64_t> _recorded{ 0 };
        std::thread _worker;
    };
}

#endif
