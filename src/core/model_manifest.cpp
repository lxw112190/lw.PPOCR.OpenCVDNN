#include <lw/ppocr/core/model_manifest.hpp>

#include <opencv2/core.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace lw::ppocr::core {
namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open model manifest: " + PathToUtf8(path));
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<int> ReadShape(const cv::FileNode& stage) {
    std::vector<int> shape;
    const cv::FileNode node = stage["input_shape"];
    if (!node.empty() && node.isSeq()) {
        for (const cv::FileNode& value : node) {
            shape.push_back(static_cast<int>(value));
        }
    }
    return shape;
}

ModelStage ReadArtifactStage(
    const cv::FileNode& stages,
    const char* stage_name,
    const std::filesystem::path& base,
    const std::string& artifact_key,
    bool required) {
    const cv::FileNode stage = stages[stage_name];
    if (stage.empty()) {
        if (required) {
            throw std::runtime_error(std::string("model manifest is missing stage: ") +
                stage_name);
        }
        return {};
    }

    const cv::FileNode artifact = stage["artifacts"][artifact_key];
    if (artifact.empty()) {
        if (required) {
            throw std::runtime_error(std::string("stage has no '") + artifact_key +
                "' artifact: " + stage_name);
        }
        return {};
    }

    std::string relative_path;
    artifact["path"] >> relative_path;
    if (relative_path.empty()) {
        throw std::runtime_error(artifact_key + " artifact path is empty: " +
            stage_name);
    }

    ModelStage result;
    result.path = (base / std::filesystem::u8path(relative_path)).lexically_normal();
    result.input_shape = ReadShape(stage);
    if (!std::filesystem::is_regular_file(result.path)) {
        throw std::runtime_error("model file does not exist: " + PathToUtf8(result.path));
    }
    return result;
}

}  // namespace

std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("failed to open file: " + PathToUtf8(path));
    }
    const std::streamsize size = input.tellg();
    if (size <= 0) {
        throw std::runtime_error("file is empty: " + PathToUtf8(path));
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("failed to read file: " + PathToUtf8(path));
    }
    return bytes;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    return path.u8string();
}

ModelManifest LoadModelManifest(
    const std::string& path_utf8,
    const std::string& artifact_key) {
    const std::filesystem::path path = std::filesystem::u8path(path_utf8);
    const std::string json = ReadTextFile(path);
    cv::FileStorage storage(json,
        cv::FileStorage::READ | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
    if (!storage.isOpened()) {
        throw std::runtime_error("failed to parse model manifest JSON: " + path_utf8);
    }

    int schema_version = 0;
    storage["schema_version"] >> schema_version;
    if (schema_version != 1) {
        throw std::runtime_error("unsupported model manifest schema_version");
    }

    ModelManifest manifest;
    storage["name"] >> manifest.name;
    if (manifest.name.empty()) {
        throw std::runtime_error("model manifest name is empty");
    }

    std::string dictionary_path;
    storage["dictionary"]["path"] >> dictionary_path;
    if (dictionary_path.empty()) {
        throw std::runtime_error("model manifest dictionary path is empty");
    }

    const std::filesystem::path base = path.parent_path();
    manifest.dictionary =
        (base / std::filesystem::u8path(dictionary_path)).lexically_normal();
    if (!std::filesystem::is_regular_file(manifest.dictionary)) {
        throw std::runtime_error("dictionary file does not exist: " +
            PathToUtf8(manifest.dictionary));
    }

    const cv::FileNode stages = storage["stages"];
    manifest.detector = ReadArtifactStage(
        stages, "detector", base, artifact_key, true);
    manifest.recognizer = ReadArtifactStage(
        stages, "recognizer", base, artifact_key, true);
    manifest.classifier = ReadArtifactStage(
        stages, "classifier", base, artifact_key, false);
    manifest.has_classifier = !manifest.classifier.path.empty();

    if (manifest.recognizer.input_shape.size() != 4 ||
        manifest.recognizer.input_shape[2] <= 0 ||
        manifest.recognizer.input_shape[3] <= 0) {
        throw std::runtime_error("recognizer input_shape must contain N,C,H,W");
    }
    if (manifest.has_classifier &&
        (manifest.classifier.input_shape.size() != 4 ||
         manifest.classifier.input_shape[2] <= 0 ||
         manifest.classifier.input_shape[3] <= 0)) {
        throw std::runtime_error("classifier input_shape must contain N,C,H,W");
    }
    return manifest;
}

ModelManifest LoadModelManifest(const std::string& path_utf8) {
    return LoadModelManifest(path_utf8, "onnx");
}

}  // namespace lw::ppocr::core
