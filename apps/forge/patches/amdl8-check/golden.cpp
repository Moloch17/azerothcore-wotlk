// mod-animus's MlpPolicy against the learner's golden vectors (apps/forge/python/tests/golden/seat_sets*): every
// case's logits, relative to action 0, within the file's tolerance; a version 7 model loads, a version 9 one is
// refused. Built and run by run.py; the inputs are prep.py's.
#include "MlpPolicy.h"
#include <algorithm>
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
            for (uint32 action = 0; action < ACTIONS; ++action)
                worst = std::max(worst, std::fabs((logits[action] - logits[0]) - entry.Logits[action]));
            ++checked;
        }
        return worst;
    }
}

int main(int argc, char** argv)
{
    if (argc != 8)
    {
        std::printf("usage: golden <model> <cases> <recurrent model> <recurrent cases> <v7 model> <v9 model> "
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
        float const worst = MaxError(policy, ReadCases(argv[2 + 2 * index]), checked);
        bool const pass = checked > 0 && worst >= 0.0f && worst <= tolerance;
        std::printf("%s (%s): %u decisions, max abs logit error %.3g (tolerance %g): %s\n", labels[index],
            policy.Describe().c_str(), checked, worst, tolerance, pass ? "ok" : "FAILED");
        ok = ok && pass;
    }

    Animus::MlpPolicy old;
    bool const v7 = old.Load(argv[5], "solo", 9, 3, error);
    std::printf("version 7 load: %s\n", v7 ? ("ok, " + old.Describe()).c_str() : error.c_str());
    Animus::MlpPolicy bad;
    bool const v9 = bad.Load(argv[6], "warrior_dps_pack", OBS, ACTIONS, error);
    std::printf("version 9 refused: %s\n", v9 ? "NO, it loaded" : error.c_str());
    return (ok && v7 && !v9) ? 0 : 1;
}
