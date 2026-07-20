#include "tracking_replay.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char **argv)
{
    std::string session, mode_text = "recorded", profile_path, output; bool allow_mismatch = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--session" && i + 1 < argc) session = argv[++i];
        else if (arg == "--mode" && i + 1 < argc) mode_text = argv[++i];
        else if (arg == "--resolved-profile" && i + 1 < argc) profile_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output = argv[++i];
        else if (arg == "--allow-fixed-detector-mismatch") allow_mismatch = true;
        else { std::cerr << "unknown or incomplete argument: " << arg << std::endl; return 2; }
    }
    if (session.empty()) { std::cerr << "--session is required" << std::endl; return 2; }
    ReplayMode mode = ReplayMode::Recorded;
    if (mode_text == "tracker-rerun") mode = ReplayMode::TrackerRerun;
    else if (mode_text == "policy-tracker-rerun") mode = ReplayMode::PolicyTrackerRerun;
    else if (mode_text != "recorded") { std::cerr << "unsupported replay mode" << std::endl; return 2; }
    ReplaySource source; std::string error;
    if (!load_replay_source(session, &source, &error)) { std::cerr << error << std::endl; return 1; }
    ResolvedTrackingProfile profile = source.source_profile;
    if (!profile_path.empty()) { std::ifstream input(profile_path.c_str()); std::ostringstream text; text << input.rdbuf(); if (!resolve_tracking_profile_json(text.str(), &profile, &error)) { std::cerr << error << std::endl; return 1; } }
    ReplayRunResult result;
    if (!run_tracking_replay(source, mode, profile, allow_mismatch, &result, &error)) { std::cerr << error << std::endl; return 1; }
    if (!output.empty() && !write_replay_output(output, source, mode, profile, result, &error)) { std::cerr << error << std::endl; return 1; }
    std::cout << "replay completed: mode=" << replay_mode_to_string(mode) << " records=" << result.frames.size()
              << " detector_comparability=" << result.detector_comparability << std::endl; return 0;
}
