#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "analysis_record.h"
#include "sha256.h"
#include "tracking_profile.h"

namespace
{
std::string read_file(const std::string &path)
{
    std::ifstream input(path.c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: v351_contracts_test <fixture-dir>" << std::endl;
        return 2;
    }
    const std::string fixture_dir = argv[1];
    bool ok = true;
    ok &= expect(kAnalysisRecordSchemaVersion == 1, "analysis record schema version");
    ok &= expect(std::string(observation_origin_to_string(ObservationOrigin::LowOnly)) == "low_only",
                 "low-only origin string");
    ok &= expect(std::string(observation_rejection_reason_to_string(
                     ObservationRejectionReason::ClassNotAllowed)) == "class_not_allowed",
                 "rejection reason string");
    ok &= expect(std::string(recorded_image_state_to_string(RecordedImageState::Dropped)) == "dropped",
                 "image state string");
    ok &= expect(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "SHA-256 known answer");

    ResolvedTrackingProfile profile;
    std::string error;
    ok &= expect(resolve_tracking_profile_json(read_file(fixture_dir + "/v351_profile_source.json"),
                                               &profile, &error),
                 std::string("valid profile resolves: ") + error);
    const nlohmann::json golden = nlohmann::json::parse(read_file(fixture_dir + "/v351_profile_golden.json"));
    const std::string actual_canonical = canonical_tracking_profile_json(profile);
    const std::string expected_canonical = golden.at("canonical_json").get<std::string>();
    if (actual_canonical != expected_canonical)
        std::cerr << "actual canonical: " << actual_canonical << std::endl;
    ok &= expect(actual_canonical == expected_canonical,
                 "C++ canonical JSON matches golden");
    ok &= expect(hash_canonical_tracking_profile(profile) == golden.at("profile_hash").get<std::string>(),
                 "C++ profile hash matches golden");

    ResolvedTrackingProfile invalid;
    ok &= expect(!resolve_tracking_profile_json(read_file(fixture_dir + "/v351_profile_invalid.json"),
                                                &invalid, &error),
                 "invalid profile is rejected");
    ok &= expect(error.find("unique") != std::string::npos,
                 "invalid profile has field-specific duplicate error");

    const nlohmann::json session = nlohmann::json::parse(
        read_file(fixture_dir + "/v351_recording_session.json"));
    const nlohmann::json replay = nlohmann::json::parse(
        read_file(fixture_dir + "/v351_replay_manifest.json"));
    ok &= expect(session.at("manifest_schema_version").get<int>() == kRecordingManifestSchemaVersion,
                 "recording manifest fixture version");
    ok &= expect(replay.at("replay_manifest_schema_version").get<int>() == kReplayManifestSchemaVersion,
                 "replay manifest fixture version");

    if (!ok)
        return 1;
    std::cout << "V3.5.1 contract fixtures passed" << std::endl;
    return 0;
}
