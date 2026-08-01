#include <lw/ppocr/core/model_manifest.hpp>

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>

using json = nlohmann::json;

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

void RequireObject(const json& value, const std::string& context) {
    if (!value.is_object()) {
        throw std::runtime_error(context + " must be an object");
    }
}

void RejectUnknownProperties(
    const json& value,
    std::initializer_list<const char*> allowed,
    const std::string& context) {
    RequireObject(value, context);
    for (auto item = value.begin(); item != value.end(); ++item) {
        const bool known = std::any_of(allowed.begin(), allowed.end(),
            [&](const char* name) { return item.key() == name; });
        if (!known) {
            throw std::runtime_error(context + " contains unknown property: " +
                item.key());
        }
    }
}

const json& RequireProperty(
    const json& value,
    const char* name,
    const std::string& context) {
    const auto item = value.find(name);
    if (item == value.end()) {
        throw std::runtime_error(context + " is missing property: " + name);
    }
    return *item;
}

std::string RequireString(
    const json& value,
    const char* name,
    const std::string& context) {
    const json& property = RequireProperty(value, name, context);
    if (!property.is_string() || property.get_ref<const std::string&>().empty()) {
        throw std::runtime_error(context + "." + name +
            " must be a non-empty string");
    }
    return property.get<std::string>();
}

void ValidateOptionalString(
    const json& value,
    const char* name,
    const std::string& context,
    bool allow_empty) {
    const auto item = value.find(name);
    if (item == value.end()) return;
    if (!item->is_string() ||
        (!allow_empty && item->get_ref<const std::string&>().empty())) {
        throw std::runtime_error(context + "." + name +
            " must be a string" + (allow_empty ? std::string{} : " and not empty"));
    }
}

void ValidateSha256(
    const json& value,
    const std::string& context) {
    const auto item = value.find("sha256");
    if (item == value.end()) return;
    if (!item->is_string()) {
        throw std::runtime_error(context + ".sha256 must be a string");
    }
    const std::string digest = item->get<std::string>();
    if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(),
        [](unsigned char character) { return std::isxdigit(character) != 0; })) {
        throw std::runtime_error(context +
            ".sha256 must contain exactly 64 hexadecimal characters");
    }
}

std::vector<int> ReadShape(
    const json& stage,
    const std::string& context) {
    const json& node = RequireProperty(stage, "input_shape", context);
    if (!node.is_array() || node.size() < 3 || node.size() > 4) {
        throw std::runtime_error(context +
            ".input_shape must contain three or four integers");
    }
    std::vector<int> shape;
    shape.reserve(node.size());
    for (const json& value : node) {
        if (!value.is_number_integer()) {
            throw std::runtime_error(context +
                ".input_shape must contain only integers");
        }
        shape.push_back(value.get<int>());
    }
    return shape;
}

ModelStage ReadArtifactStage(
    const json& stages,
    const char* stage_name,
    const std::filesystem::path& base,
    bool required) {
    const auto stage_item = stages.find(stage_name);
    if (stage_item == stages.end()) {
        if (required) {
            throw std::runtime_error(std::string(
                "model manifest is missing stage: ") + stage_name);
        }
        return {};
    }

    const std::string context = std::string("stages.") + stage_name;
    const json& stage = *stage_item;
    RejectUnknownProperties(stage,
        {"input_name", "output_name", "input_shape", "artifacts"}, context);
    ValidateOptionalString(stage, "input_name", context, true);
    ValidateOptionalString(stage, "output_name", context, true);

    ModelStage result;
    result.input_shape = ReadShape(stage, context);

    const json& artifacts = RequireProperty(stage, "artifacts", context);
    RejectUnknownProperties(artifacts, {"onnx"}, context + ".artifacts");
    const json& artifact = RequireProperty(
        artifacts, "onnx", context + ".artifacts");
    RejectUnknownProperties(artifact,
        {"path", "format", "precision", "sha256"},
        context + ".artifacts.onnx");

    const std::string relative_path = RequireString(
        artifact, "path", context + ".artifacts.onnx");
    const std::string format = RequireString(
        artifact, "format", context + ".artifacts.onnx");
    if (format != "onnx") {
        throw std::runtime_error(context +
            ".artifacts.onnx.format must be 'onnx'");
    }
    const std::string precision = RequireString(
        artifact, "precision", context + ".artifacts.onnx");
    if (precision != "fp32" && precision != "fp16" && precision != "int8") {
        throw std::runtime_error(context +
            ".artifacts.onnx.precision is unsupported");
    }
    ValidateSha256(artifact, context + ".artifacts.onnx");

    result.path =
        (base / std::filesystem::u8path(relative_path)).lexically_normal();
    if (!std::filesystem::is_regular_file(result.path)) {
        throw std::runtime_error("model file does not exist: " +
            PathToUtf8(result.path));
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

ModelManifest LoadModelManifest(const std::string& path_utf8) {
    const std::filesystem::path path = std::filesystem::u8path(path_utf8);
    const std::string source = ReadTextFile(path);
    json document;
    try {
        document = json::parse(source);
    } catch (const json::parse_error& error) {
        throw std::runtime_error(std::string(
            "failed to parse model manifest JSON: ") + error.what());
    }

    RejectUnknownProperties(document,
        {"$schema", "schema_version", "name", "family", "language",
         "dictionary", "stages"},
        "model manifest");
    ValidateOptionalString(document, "$schema", "model manifest", false);

    const json& schema_version = RequireProperty(
        document, "schema_version", "model manifest");
    if (!schema_version.is_number_integer() || schema_version.get<int>() != 1) {
        throw std::runtime_error("unsupported model manifest schema_version");
    }

    ModelManifest manifest;
    manifest.name = RequireString(document, "name", "model manifest");
    RequireString(document, "family", "model manifest");
    ValidateOptionalString(document, "language", "model manifest", false);

    const json& dictionary = RequireProperty(
        document, "dictionary", "model manifest");
    RejectUnknownProperties(dictionary, {"path", "sha256"}, "dictionary");
    const std::string dictionary_path = RequireString(
        dictionary, "path", "dictionary");
    ValidateSha256(dictionary, "dictionary");

    const std::filesystem::path base = path.parent_path();
    manifest.dictionary =
        (base / std::filesystem::u8path(dictionary_path)).lexically_normal();
    if (!std::filesystem::is_regular_file(manifest.dictionary)) {
        throw std::runtime_error("dictionary file does not exist: " +
            PathToUtf8(manifest.dictionary));
    }

    const json& stages = RequireProperty(document, "stages", "model manifest");
    RejectUnknownProperties(stages,
        {"detector", "classifier", "recognizer"}, "stages");
    manifest.detector = ReadArtifactStage(
        stages, "detector", base, true);
    manifest.recognizer = ReadArtifactStage(
        stages, "recognizer", base, true);
    manifest.classifier = ReadArtifactStage(
        stages, "classifier", base, false);
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

ModelManifest LoadModelManifest(
    const std::string& path_utf8,
    const std::string& artifact_key) {
    if (artifact_key != "onnx") {
        throw std::runtime_error("model manifest schema v1 only supports onnx artifacts");
    }
    return LoadModelManifest(path_utf8);
}

}  // namespace lw::ppocr::core
