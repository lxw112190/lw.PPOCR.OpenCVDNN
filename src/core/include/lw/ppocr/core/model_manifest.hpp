#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace lw::ppocr::core {

struct ModelStage {
    std::filesystem::path path;
    std::vector<int> input_shape;
};

struct ModelManifest {
    std::string name;
    std::filesystem::path dictionary;
    ModelStage detector;
    ModelStage classifier;
    ModelStage recognizer;
    bool has_classifier = false;
};

ModelManifest LoadModelManifest(const std::string& path_utf8);
ModelManifest LoadModelManifest(
    const std::string& path_utf8,
    const std::string& artifact_key);
std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path& path);
std::string PathToUtf8(const std::filesystem::path& path);

}  // namespace lw::ppocr::core
