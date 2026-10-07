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

#include "EvalVideo.h"
#include "FrameImage.h"
#include "Log.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

namespace Vi = Animus::Vision;

/// One evaluation: where its videos go and, once written, their sidecars (the worker's).
struct Vi::EvalVideoRecorder::Evaluation
{
    EvalVideoOptions Options;
    std::vector<uint32_t> Seeds;
    boost::json::array Videos;
};

/// One episode's video.
struct Vi::EvalVideoRecorder::Reel
{
    EvalVideoEpisode Episode;
    // The world thread's until the Close job is queued, then the worker's.
    uint32_t Frames = 0;
    uint32_t Dropped = 0;
    bool Truncated = false;
    // The worker's.
    std::unique_ptr<ApngWriter> Writer;
    uint32_t Bad = 0;
    uint64_t EncodeNs = 0;
};

namespace
{
    bool EndsWith(std::string const& text, std::string const& suffix)
    {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    /// A layout's name as a file name's part: lower case letters, digits and '_'.
    std::string FileWord(std::string const& text)
    {
        std::string out;
        for (char c : text)
            out += std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_';
        return out.empty() ? std::string("seat") : out;
    }

    bool WriteFile(std::filesystem::path const& path, std::string const& body)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << body;
        return bool(out);
    }

    std::string HtmlEscape(std::string const& text)
    {
        std::string out;
        for (char c : text)
            out += c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : c == '&' ? std::string("&amp;")
                : std::string(1, c);
        return out;
    }
}

std::vector<uint32_t> Vi::EvalVideoSeeds(uint32_t first, uint32_t end, uint32_t count, uint32_t pairs)
{
    if (end <= first || !count)
        return {};
    uint32_t const episodes = end - first;
    pairs = std::max<uint32_t>(1, pairs);
    count = std::min(count, episodes);
    // Whole cycles of the pairs the range holds: the rungs the picks spread over.
    uint32_t const cycles = std::max<uint32_t>(1, episodes / pairs);

    std::vector<uint32_t> seeds;
    for (uint32_t k = 0; k < count; ++k)
    {
        uint64_t const pair = uint64_t(k) * pairs / count;
        uint64_t const cycle = uint64_t(k) * cycles / count;
        uint32_t offset = uint32_t((cycle * pairs + pair) % episodes);
        // A pick that lands on one already taken (a range shorter than a cycle) moves on to the next free seed.
        while (std::find(seeds.begin(), seeds.end(), first + offset) != seeds.end())
            offset = (offset + 1) % episodes;
        seeds.push_back(first + offset);
    }
    std::sort(seeds.begin(), seeds.end());
    return seeds;
}

std::string Vi::EvalVideoOutcome(std::vector<std::string> const& names, float const* row)
{
    if (!row)
        return "unfinished";
    for (char const* outcome : EVAL_VIDEO_OUTCOMES)
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == outcome)
                return row[i] > 0.5f ? "success" : "failure";
    return "ended";
}

int32_t Vi::EvalVideoRungColumn(std::vector<std::string> const& names)
{
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        std::string const& name = names[i];
        if (name == "rung" || name == "tier" || EndsWith(name, "_rung") || EndsWith(name, "_tier"))
            return int32_t(i);
    }
    return -1;
}

Vi::EvalVideoRecorder::~EvalVideoRecorder()
{
    End();
    {
        std::lock_guard<std::mutex> guard(_lock);
        _stop = true;
    }
    _wake.notify_all();
    if (_worker.joinable())
        _worker.join();
}

void Vi::EvalVideoRecorder::Begin(EvalVideoOptions options, std::vector<uint32_t> seeds)
{
    End();
    if (seeds.empty())
        return;
    _evaluation = std::make_shared<Evaluation>();
    _evaluation->Options = std::move(options);
    _evaluation->Seeds = seeds;
    _wanted = std::move(seeds);
}

void Vi::EvalVideoRecorder::End()
{
    if (!_evaluation)
        return;
    std::vector<uint32_t> envs;
    for (auto const& [env, film] : _open)
        envs.push_back(env);
    std::sort(envs.begin(), envs.end());
    for (uint32_t env : envs)
        Finish(env, nullptr);
    Job index;
    index.What = Job::Kind::Index;
    index.Of = std::move(_evaluation);
    Push(std::move(index));
    _evaluation.reset();
    _wanted.clear();
}

bool Vi::EvalVideoRecorder::Wanted(uint32_t seed) const
{
    return _evaluation && std::find(_wanted.begin(), _wanted.end(), seed) != _wanted.end();
}

uint32_t Vi::EvalVideoRecorder::Seed(uint32_t env) const
{
    auto const itr = _open.find(env);
    return itr == _open.end() ? UINT32_MAX : itr->second->Episode.Seed;
}

uint32_t Vi::EvalVideoRecorder::Agent(uint32_t env) const
{
    auto const itr = _open.find(env);
    return itr == _open.end() ? 0 : itr->second->Episode.Agent;
}

void Vi::EvalVideoRecorder::Start(EvalVideoEpisode episode)
{
    if (!_evaluation)
        return;
    _wanted.erase(std::remove(_wanted.begin(), _wanted.end(), episode.Seed), _wanted.end());
    // A recording left open on the env (its end never seen) is closed first, as unfinished.
    if (Recording(episode.Env))
        Finish(episode.Env, nullptr);
    auto film = std::make_shared<Reel>();
    uint32_t const env = episode.Env;
    film->Episode = std::move(episode);
    _open[env] = std::move(film);
}

void Vi::EvalVideoRecorder::Frame(uint32_t env, uint8_t const* image, uint8_t const* map)
{
    auto const itr = _open.find(env);
    if (itr == _open.end() || !image)
        return;
    Reel& film = *itr->second;
    EvalVideoOptions const& options = _evaluation->Options;
    if (options.MaxFrames && film.Frames >= options.MaxFrames)
    {
        film.Truncated = true;
        return;
    }

    std::size_t const bytes = std::size_t(options.ImageBytes) + (map ? options.MapBytes : 0);
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_queuedBytes + bytes > MAX_QUEUED_BYTES)
        {
            ++film.Dropped;
            ++_droppedTotal;
            return;
        }
    }

    Job job;
    job.What = Job::Kind::Frame;
    job.Of = _evaluation;
    job.Film = itr->second;
    job.Bytes.resize(bytes);
    std::copy_n(image, options.ImageBytes, job.Bytes.data());
    if (map && options.MapBytes)
        std::copy_n(map, options.MapBytes, job.Bytes.data() + options.ImageBytes);
    ++film.Frames;
    Push(std::move(job));
}

void Vi::EvalVideoRecorder::Finish(uint32_t env, float const* info)
{
    auto const itr = _open.find(env);
    if (itr == _open.end())
        return;
    Job job;
    job.What = Job::Kind::Close;
    job.Of = _evaluation;
    job.Film = std::move(itr->second);
    _open.erase(itr);
    if (info)
        job.Info.assign(info, info + job.Of->Options.InfoNames.size());
    Push(std::move(job));
}

void Vi::EvalVideoRecorder::Drain()
{
    std::unique_lock<std::mutex> guard(_lock);
    _idle.wait(guard, [this]() { return _jobs.empty() && !_busy; });
}

std::string Vi::EvalVideoRecorder::LastDir() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _lastDir;
}

void Vi::EvalVideoRecorder::Push(Job job)
{
    {
        std::lock_guard<std::mutex> guard(_lock);
        _queuedBytes += job.Bytes.size();
        _jobs.push_back(std::move(job));
        if (!_worker.joinable())
            _worker = std::thread([this]() { Work(); });
    }
    _wake.notify_one();
}

void Vi::EvalVideoRecorder::Work()
{
    std::unique_lock<std::mutex> guard(_lock);
    while (true)
    {
        _wake.wait(guard, [this]() { return _stop || !_jobs.empty(); });
        if (_jobs.empty())
            return;     // stopping, with everything written
        Job job = std::move(_jobs.front());
        _jobs.pop_front();
        _busy = true;
        guard.unlock();

        if (job.What == Job::Kind::Frame)
            WriteFrame(job);
        else if (job.What == Job::Kind::Close)
            WriteVideo(job);
        else
            WriteIndex(*job.Of);

        guard.lock();
        _queuedBytes -= job.Bytes.size();
        _busy = false;
        if (_jobs.empty())
            _idle.notify_all();
    }
}

void Vi::EvalVideoRecorder::WriteFrame(Job const& job)
{
    auto const started = std::chrono::steady_clock::now();
    EvalVideoOptions const& options = job.Of->Options;
    Reel& film = *job.Film;
    uint8_t const* map = job.Bytes.size() > options.ImageBytes ? job.Bytes.data() + options.ImageBytes : nullptr;
    RgbImage const frame = CompositeRgb(options.Camera, job.Bytes.data(), options.Scale, map);
    if (!film.Writer)
        film.Writer = std::make_unique<ApngWriter>(frame.Width, frame.Height,
            uint16_t(std::clamp<uint32_t>(options.DecisionMs, 1, 65535)), uint16_t(1000));
    if (!film.Writer->Add(frame))
        ++film.Bad;
    film.EncodeNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
}

void Vi::EvalVideoRecorder::WriteVideo(Job const& job)
{
    namespace fs = std::filesystem;
    Evaluation& evaluation = *job.Of;
    EvalVideoOptions const& options = evaluation.Options;
    Reel& film = *job.Film;
    EvalVideoEpisode const& episode = film.Episode;
    std::vector<std::string> const& names = options.InfoNames;
    bool const ended = !job.Info.empty() && job.Info.size() == names.size();
    std::string const outcome = EvalVideoOutcome(names, ended ? job.Info.data() : nullptr);

    if (!film.Writer || !film.Writer->Frames())
    {
        LOG_WARN("module.animus", "Eval videos: seed {} ({}) has no frames to write", episode.Seed, episode.Layout);
        return;
    }

    std::error_code error;
    fs::create_directories(options.Dir, error);
    std::string const stem = Acore::StringFormat("{}-{}-{}", episode.Seed, FileWord(episode.Layout), outcome);
    std::string const video = film.Writer->Finish();
    if (video.empty() || !WriteFile(options.Dir / (stem + ".png"), video))
    {
        LOG_WARN("module.animus", "Eval videos: cannot write {}", (options.Dir / (stem + ".png")).string());
        film.Writer.reset();
        return;
    }

    boost::json::object sidecar;
    sidecar["file"] = stem + ".png";
    sidecar["scenario"] = options.Scenario;
    sidecar["evaluation"] = options.Label;
    sidecar["seed"] = episode.Seed;
    sidecar["env"] = episode.Env;
    sidecar["agent"] = episode.Agent;
    sidecar["layout"] = episode.Layout;
    sidecar["class"] = episode.Class;
    sidecar["race"] = episode.Race;
    sidecar["level"] = episode.Level;
    int32_t const rung = EvalVideoRungColumn(names);
    if (ended && rung >= 0)
        sidecar["rung"] = job.Info[rung];
    else
        sidecar["rung"] = nullptr;
    sidecar["outcome"] = outcome;
    sidecar["frames"] = film.Writer->Frames();
    sidecar["dropped_frames"] = film.Dropped;
    sidecar["bad_frames"] = film.Bad;
    sidecar["truncated"] = film.Truncated;
    sidecar["decision_ms"] = options.DecisionMs;
    sidecar["fps"] = 1000.0 / double(std::max<uint32_t>(1, options.DecisionMs));
    sidecar["width"] = options.Camera.Width * options.Scale;
    sidecar["height"] = options.Camera.Height * options.Scale;
    sidecar["scale"] = options.Scale;
    sidecar["camera"] = Acore::StringFormat("{}x{}", options.Camera.Width, options.Camera.Height);
    if (episode.RenderWidth && episode.RenderHeight)
        sidecar["render"] = Acore::StringFormat("{}x{}", episode.RenderWidth, episode.RenderHeight);
    else
        sidecar["render"] = nullptr;
    sidecar["map_inset"] = options.MapBytes > 0;
    sidecar["bytes"] = uint64_t(video.size());
    sidecar["encode_ms_per_frame"] = double(film.EncodeNs) / 1e6 / double(film.Writer->Frames());
    // The episode's measures, every column it reported (the zeros left out: most of a stage's columns are other
    // stages' or other seats').
    boost::json::object measures;
    if (ended)
        for (std::size_t i = 0; i < names.size(); ++i)
            if (job.Info[i] != 0.0f)
                measures[names[i]] = job.Info[i];
    sidecar["measures"] = std::move(measures);

    if (!WriteFile(options.Dir / (stem + ".json"), boost::json::serialize(sidecar) + "\n"))
        LOG_WARN("module.animus", "Eval videos: cannot write {}", (options.Dir / (stem + ".json")).string());
    evaluation.Videos.push_back(std::move(sidecar));
    film.Writer.reset();
    _recorded.fetch_add(1, std::memory_order_relaxed);
}

void Vi::EvalVideoRecorder::WriteIndex(Evaluation& evaluation)
{
    namespace fs = std::filesystem;
    EvalVideoOptions const& options = evaluation.Options;
    std::error_code error;
    fs::create_directories(options.Dir, error);

    boost::json::object index;
    index["scenario"] = options.Scenario;
    index["evaluation"] = options.Label;
    boost::json::array seeds;
    for (uint32_t seed : evaluation.Seeds)
        seeds.push_back(seed);
    index["selected_seeds"] = std::move(seeds);
    index["videos"] = evaluation.Videos;
    if (!WriteFile(options.Dir / "index.json", boost::json::serialize(index) + "\n"))
        LOG_WARN("module.animus", "Eval videos: cannot write {}", (options.Dir / "index.json").string());

    // A page to watch them side by side: every video loops in a browser as it is.
    std::string html = "<!doctype html><meta charset=\"utf-8\"><title>" + HtmlEscape(options.Scenario + " "
        + options.Label) + "</title><style>body{background:#111;color:#ddd;font:13px sans-serif}figure{display:"
        "inline-block;margin:8px}img{image-rendering:pixelated;max-width:100%}</style>\n";
    for (boost::json::value const& video : evaluation.Videos)
    {
        boost::json::object const& row = video.as_object();
        std::string const file = std::string(row.at("file").as_string());
        html += "<figure><img src=\"" + HtmlEscape(file) + "\"><figcaption>" + HtmlEscape(Acore::StringFormat(
            "seed {} {} -- {} ({} frames)", row.at("seed").to_number<uint64_t>(), std::string(row.at("layout").as_string()),
            std::string(row.at("outcome").as_string()), row.at("frames").to_number<uint64_t>())) + "</figcaption></figure>\n";
    }
    if (!WriteFile(options.Dir / "index.html", html))
        LOG_WARN("module.animus", "Eval videos: cannot write {}", (options.Dir / "index.html").string());

    {
        std::lock_guard<std::mutex> guard(_lock);
        _lastDir = options.Dir.string();
    }
    LOG_INFO("module.animus", "Eval videos: {} recorded to {}", evaluation.Videos.size(), options.Dir.string());
}
