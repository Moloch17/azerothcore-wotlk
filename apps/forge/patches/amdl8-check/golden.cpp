// mod-animus's MlpPolicy against the learner's golden vectors (apps/forge/python/tests/golden/): every case's
// logits, relative to action 0, within the file's tolerance; a version 7 model loads, one of a version past the
// reader's is refused; and what a decision costs. Built and run by run.py, once per golden set; the inputs are
// prep.py's.
#include "MlpPolicy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint32 OBS = 20;
    constexpr uint32 ACTIONS = 4;

    struct Case
    {
        int Sequence = -1;      // -1: a lone decision; otherwise the recurrent sequence it steps
        std::vector<float> Obs, Logits;
    };

    // A line: "<sequence> obs[OBS] logits[ACTIONS]".
    std::vector<Case> ReadCases(char const* path)
    {
        std::vector<Case> cases;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream row(line);
            Case entry;
            entry.Obs.resize(OBS);
            entry.Logits.resize(ACTIONS);
            row >> entry.Sequence;
            for (float& v : entry.Obs)
                row >> v;
            for (float& v : entry.Logits)
                row >> v;
            cases.push_back(std::move(entry));
        }
        return cases;
    }

    // The largest error of `policy` over `cases`, a fresh memory at each sequence's start; -1 when it cannot decide.
    float MaxError(Animus::MlpPolicy& policy, std::vector<Case> const& cases, uint32& checked)
    {
        Animus::MlpPolicy::State state;
        int sequence = -2;
        float worst = 0.0f;
        std::vector<float> logits(ACTIONS);
        for (Case const& entry : cases)
        {
            if (entry.Sequence != sequence)
            {
                state.Clear();
                sequence = entry.Sequence;
            }
            if (!policy.Logits(entry.Obs.data(), logits.data(), &state))
                return -1.0f;
            float error = 0.0f;
            for (uint32 action = 0; action < ACTIONS; ++action)
                error = std::max(error, std::fabs((logits[action] - logits[0]) - entry.Logits[action]));
            if (error > 1e-4f)
                std::printf("  decision %u: error %.3g\n", checked, error);
            worst = std::max(worst, error);
            ++checked;
        }
        return worst;
    }

    // Microseconds a Decide takes, over the cases repeated (the realm runs it on the CPU, one seat at a time).
    double DecideMicros(Animus::MlpPolicy& policy, std::vector<Case> const& cases)
    {
        constexpr int REPEATS = 2000;
        std::vector<uint8> mask(ACTIONS, 1);
        Animus::MlpPolicy::State state;
        int sink = 0;
        auto const start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < REPEATS; ++repeat)
            for (Case const& entry : cases)
                sink += policy.Decide(entry.Obs.data(), mask.data(), &state);
        auto const elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
        return sink < 0 ? 0.0 : elapsed.count() / double(REPEATS * cases.size());
    }
}

int main(int argc, char** argv)
{
    // "bench <model> <scenario> <obs> <actions>": what a decision costs at a model's real size, on random inputs.
    if (argc == 6 && std::string(argv[1]) == "bench")
    {
        Animus::MlpPolicy policy;
        std::string error;
        uint32 const obs = uint32(std::stoul(argv[4]));
        uint32 const actions = uint32(std::stoul(argv[5]));
        if (!policy.Load(argv[2], argv[3], obs, actions, error))
        {
            std::printf("load failed: %s\n", error.c_str());
            return 1;
        }
        std::vector<float> input(obs);
        std::vector<uint8> mask(actions, 1);
        Animus::MlpPolicy::State state;
        uint32 seed = 1;
        for (float& v : input)
            v = float((seed = seed * 1103515245u + 12345u) >> 16 & 1);     // present flags and features alike
        constexpr int DECISIONS = 2000;
        int sink = 0;
        auto const start = std::chrono::steady_clock::now();
        for (int i = 0; i < DECISIONS; ++i)
            sink += policy.Decide(input.data(), mask.data(), &state);
        auto const elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
        std::printf("%s (%s): %.1f us a decision%s\n", argv[3], policy.Describe().c_str(),
            elapsed.count() / DECISIONS, sink < 0 ? "" : "");
        return 0;
    }
    if (argc != 8)
    {
        std::printf("usage: golden <model> <cases> <recurrent model> <recurrent cases> <v7 model> <future model> "
            "<tolerance>\n");
        return 2;
    }
    float const tolerance = std::stof(argv[7]);
    bool ok = true;
    std::string error;
    char const* labels[] = { "lone decisions", "recurrent sequences" };
    for (int index = 0; index < 2; ++index)
    {
        Animus::MlpPolicy policy;
        if (!policy.Load(argv[1 + 2 * index], "warrior_dps_pack", OBS, ACTIONS, error))
        {
            std::printf("%s: load failed: %s\n", labels[index], error.c_str());
            ok = false;
            continue;
        }
        uint32 checked = 0;
        std::vector<Case> const cases = ReadCases(argv[2 + 2 * index]);
        float const worst = MaxError(policy, cases, checked);
        bool const pass = checked > 0 && worst >= 0.0f && worst <= tolerance;
        std::printf("%s (%s): %u decisions, max abs logit error %.3g (tolerance %g): %s; %.1f us a decision\n",
            labels[index], policy.Describe().c_str(), checked, worst, tolerance, pass ? "ok" : "FAILED",
            DecideMicros(policy, cases));
        ok = ok && pass;
    }

    Animus::MlpPolicy old;
    bool const v7 = old.Load(argv[5], "solo", 9, 3, error);
    std::printf("version 7 load: %s\n", v7 ? ("ok, " + old.Describe()).c_str() : error.c_str());
    Animus::MlpPolicy bad;
    bool const future = bad.Load(argv[6], "warrior_dps_pack", OBS, ACTIONS, error);
    std::printf("a version past the reader's refused: %s\n", future ? "NO, it loaded" : error.c_str());
    return (ok && v7 && !future) ? 0 : 1;
}
