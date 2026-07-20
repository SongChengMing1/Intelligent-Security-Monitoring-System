#include "tracking_replay.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "analysis_recorder.h"
#include "sha256.h"

bool load_replay_source(const std::string &session_path, ReplaySource *source, std::string *error_message)
{
    if (!source) return false;
    try
    {
        std::ifstream manifest_input((session_path + "/manifest.json").c_str(), std::ios::binary);
        std::ostringstream text; text << manifest_input.rdbuf();
        if (text.str().empty()) throw std::runtime_error("manifest.json is missing or empty");
        const nlohmann::json manifest = nlohmann::json::parse(text.str());
        ReplaySource output; output.session_path = session_path; output.manifest_text = text.str();
        output.manifest_hash = std::string("sha256:") + sha256_hex(output.manifest_text);
        output.session_id = manifest.at("recording_session_id").get<std::string>();
        output.source_git_commit = manifest.value("git_commit", "unknown");
        output.source_profile_hash = manifest.at("effective_profile_hash").get<std::string>();
        if (!resolve_tracking_profile_json(manifest.at("resolved_profile").dump(), &output.source_profile, error_message)) return false;
        SessionReadResult records; if (!read_analysis_session(session_path, &records)) throw std::runtime_error(records.error);
        output.records = records.records; *source = output; if (error_message) error_message->clear(); return true;
    }
    catch (const std::exception &exception) { if (error_message) *error_message = exception.what(); return false; }
}
