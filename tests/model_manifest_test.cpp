#include <lw/ppocr/core/model_manifest.hpp>

#include <json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class TemporaryManifest {
public:
    TemporaryManifest(const fs::path& directory, const json& document) {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = directory / (".model-manifest-test-" +
            std::to_string(suffix) + ".json");
        std::ofstream output(path_, std::ios::binary);
        output << document.dump(2);
        if (!output) {
            throw std::runtime_error("failed to write temporary manifest");
        }
    }

    ~TemporaryManifest() {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

bool ExpectFailure(
    const fs::path& directory,
    const json& document,
    const std::string& expected_message) {
    TemporaryManifest temporary(directory, document);
    try {
        lw::ppocr::core::LoadModelManifest(temporary.path().u8string());
    } catch (const std::exception& error) {
        return std::string(error.what()).find(expected_message) !=
            std::string::npos;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: model_manifest_test <model.json>\n";
        return 2;
    }

    const fs::path manifest_path = fs::absolute(fs::u8path(argv[1]));
    const auto manifest =
        lw::ppocr::core::LoadModelManifest(manifest_path.u8string());
    if (manifest.name != "PP-OCRv6 tiny Chinese" ||
        !manifest.has_classifier || manifest.detector.path.empty() ||
        manifest.recognizer.input_shape.size() != 4) {
        return 3;
    }

    std::ifstream input(manifest_path, std::ios::binary);
    json source;
    input >> source;
    if (!input) return 4;

    const fs::path model_directory = manifest_path.parent_path();
    source["dictionary"]["path"] =
        (model_directory / source["dictionary"]["path"].get<std::string>())
            .u8string();
    for (auto& stage : source["stages"].items()) {
        json& artifact = stage.value()["artifacts"]["onnx"];
        artifact["path"] =
            (model_directory / artifact["path"].get<std::string>()).u8string();
    }
    const fs::path temporary_directory = fs::temp_directory_path();

    json changed = source;
    changed["unexpected"] = true;
    if (!ExpectFailure(temporary_directory, changed,
            "unknown property: unexpected")) {
        return 5;
    }

    changed = source;
    changed.erase("family");
    if (!ExpectFailure(temporary_directory, changed,
            "missing property: family")) {
        return 6;
    }

    changed = source;
    changed["schema_version"] = 2;
    if (!ExpectFailure(temporary_directory, changed,
            "unsupported model manifest schema_version")) {
        return 7;
    }

    changed = source;
    changed["stages"]["detector"]["artifacts"]["onnx"]["format"] =
        "openvino";
    if (!ExpectFailure(temporary_directory, changed,
            "format must be 'onnx'")) {
        return 8;
    }

    changed = source;
    changed["dictionary"]["sha256"] = "not-a-digest";
    if (!ExpectFailure(temporary_directory, changed,
            "64 hexadecimal characters")) {
        return 9;
    }

    std::cout << "Model manifest schema v1 validation passed\n";
    return 0;
}
