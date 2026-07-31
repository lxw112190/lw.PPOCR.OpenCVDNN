#include <lw/ppocr.h>

#include "../engine/ocr_engine.hpp"

#include <json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct lw_ppocr_engine {
    std::unique_ptr<lw::ppocr::opencv_dnn::OcrEngine> engine;
    uint64_t max_image_pixels = 40000000;
};

namespace {

thread_local std::string g_last_error;

double Milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

uint64_t CopyText(const std::string& text, char* buffer, uint64_t capacity) {
    const uint64_t required = static_cast<uint64_t>(text.size()) + 1;
    if (buffer != nullptr && capacity > 0) {
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(capacity - 1, text.size()));
        if (count > 0) {
            std::memcpy(buffer, text.data(), count);
        }
        buffer[count] = '\0';
    }
    return required;
}

cv::Mat DecodeImage(
    const uint8_t* encoded,
    uint64_t encoded_size,
    uint64_t maximum_pixels) {
    if (encoded == nullptr || encoded_size == 0 ||
        encoded_size > static_cast<uint64_t>(
            (std::numeric_limits<int>::max)())) {
        throw std::invalid_argument("encoded image is empty or too large");
    }
    cv::Mat input(1, static_cast<int>(encoded_size), CV_8UC1,
        const_cast<uint8_t*>(encoded));
    cv::Mat image = cv::imdecode(input, cv::IMREAD_COLOR);
    if (image.empty()) {
        throw std::invalid_argument("unsupported or corrupt encoded image");
    }
    const uint64_t pixels = static_cast<uint64_t>(image.cols) * image.rows;
    if (pixels > maximum_pixels) {
        throw std::invalid_argument(
            "decoded image exceeds max_image_pixels");
    }
    return image;
}

json Timing(const lw::ppocr::core::StageTiming& value) {
    return {
        {"preprocess_ms", value.preprocess_ms},
        {"inference_ms", value.inference_ms},
        {"postprocess_ms", value.postprocess_ms},
        {"total_ms", value.total_ms}
    };
}

json OcrJson(
    const lw::ppocr::core::PipelineResult& result,
    const cv::Mat& image,
    double decode_ms) {
    json items = json::array();
    for (const auto& region : result.regions) {
        json item = {
            {"text", region.text},
            {"score", region.score},
            {"cls_label", region.cls_label},
            {"cls_score", region.cls_score},
            {"box", json::array()}
        };
        for (size_t index = 0; index < region.box.size(); ++index) {
            item["box"].push_back({
                {"x", region.box[index].x},
                {"y", region.box[index].y}
            });
        }
        items.push_back(std::move(item));
    }
    return {
        {"image", {{"width", image.cols}, {"height", image.rows}}},
        {"result", std::move(items)},
        {"timing", {
            {"decode_ms", decode_ms},
            {"detector", Timing(result.detector)},
            {"classifier", Timing(result.classifier)},
            {"recognizer", Timing(result.recognizer)},
            {"pipeline", Timing(result.pipeline)}
        }}
    };
}

json RecognitionJson(
    const lw::ppocr::core::RecognitionResult& result,
    double decode_ms) {
    json items = json::array();
    for (size_t index = 0; index < result.items.size(); ++index) {
        const auto& item = result.items[index];
        items.push_back({
            {"source_index", index},
            {"text", item.text},
            {"score", item.score},
            {"cls_label", item.cls_label},
            {"cls_score", item.cls_score}
        });
    }
    return {
        {"result", std::move(items)},
        {"timing", {
            {"decode_ms", decode_ms},
            {"classifier", Timing(result.classifier)},
            {"recognizer", Timing(result.recognizer)},
            {"pipeline", Timing(result.pipeline)}
        }}
    };
}

void AllocateJson(
    const json& document,
    char** result,
    uint64_t* result_length) {
    const std::string text = document.dump();
    char* value = static_cast<char*>(std::malloc(text.size() + 1));
    if (value == nullptr) {
        throw std::bad_alloc();
    }
    std::memcpy(value, text.c_str(), text.size() + 1);
    *result = value;
    *result_length = static_cast<uint64_t>(text.size());
}

template <typename Function>
lw_ppocr_status Guard(Function&& function, lw_ppocr_status default_status) {
    try {
        function();
        g_last_error.clear();
        return LW_PPOCR_STATUS_OK;
    } catch (const cv::Exception& exception) {
        g_last_error = std::string("OpenCV: ") + exception.what();
        return default_status;
    } catch (const std::invalid_argument& exception) {
        g_last_error = exception.what();
        return LW_PPOCR_STATUS_IMAGE_ERROR;
    } catch (const std::bad_alloc&) {
        g_last_error = "out of memory";
        return LW_PPOCR_STATUS_OUT_OF_MEMORY;
    } catch (const std::exception& exception) {
        g_last_error = exception.what();
        return default_status;
    } catch (...) {
        g_last_error = "unknown internal error";
        return LW_PPOCR_STATUS_INTERNAL_ERROR;
    }
}

bool ValidOutput(char** output, uint64_t* length) {
    if (output == nullptr || length == nullptr) {
        g_last_error = "result output arguments are required";
        return false;
    }
    *output = nullptr;
    *length = 0;
    return true;
}

}  // namespace

void LW_PPOCR_CALL lw_ppocr_config_init(lw_ppocr_config* config) {
    if (config == nullptr) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = LW_PPOCR_API_VERSION;
    config->enable_classifier = 1;
    config->limit_side_len = 960;
    config->det_db_threshold = 0.3f;
    config->det_db_box_threshold = 0.6f;
    config->det_db_unclip_ratio = 1.6f;
    config->cls_threshold = 0.9f;
    config->cls_batch_size = 8;
    config->rec_batch_size = 8;
    config->rec_concurrency = 1;
    config->max_image_pixels = 40000000;
    config->log_level = LW_PPOCR_LOG_OFF;
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_get_version(lw_ppocr_version* version) {
    if (version == nullptr || version->struct_size < sizeof(*version)) {
        g_last_error = "version structure is invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    version->api_version = LW_PPOCR_API_VERSION;
    version->major = LW_PPOCR_VERSION_MAJOR;
    version->minor = LW_PPOCR_VERSION_MINOR;
    version->patch = LW_PPOCR_VERSION_PATCH;
    version->product_name_utf8 = LW_PPOCR_PRODUCT_NAME;
    version->version_utf8 = LW_PPOCR_VERSION_STRING;
    g_last_error.clear();
    return LW_PPOCR_STATUS_OK;
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_create(
    const lw_ppocr_config* config,
    lw_ppocr_handle* handle) {
    if (config == nullptr || handle == nullptr ||
        config->struct_size < sizeof(*config) ||
        config->api_version != LW_PPOCR_API_VERSION ||
        config->model_manifest_utf8 == nullptr ||
        config->model_manifest_utf8[0] == '\0') {
        g_last_error = "OCR configuration is invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    *handle = nullptr;
    if (config->limit_side_len < 32 || config->max_image_pixels == 0 ||
        config->cls_batch_size < 1 || config->rec_batch_size < 1 ||
        config->rec_concurrency < 1) {
        g_last_error = "OCR numeric configuration is invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    return Guard([&]() {
        auto owned = std::make_unique<lw_ppocr_engine>();
        owned->max_image_pixels = config->max_image_pixels;
        owned->engine =
            std::make_unique<lw::ppocr::opencv_dnn::OcrEngine>(*config);
        *handle = owned.release();
    }, LW_PPOCR_STATUS_MODEL_ERROR);
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_ocr_encoded(
    lw_ppocr_handle handle,
    const uint8_t* encoded_image,
    uint64_t encoded_size,
    char** result_json_utf8,
    uint64_t* result_json_length) {
    if (handle == nullptr || !ValidOutput(result_json_utf8, result_json_length)) {
        if (handle == nullptr) {
            g_last_error = "OCR handle is null";
        }
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    return Guard([&]() {
        const auto decode_start = Clock::now();
        cv::Mat image = DecodeImage(encoded_image, encoded_size,
            handle->max_image_pixels);
        const double decode_ms = Milliseconds(decode_start, Clock::now());
        AllocateJson(OcrJson(handle->engine->Run(image), image, decode_ms),
            result_json_utf8, result_json_length);
    }, LW_PPOCR_STATUS_INFERENCE_ERROR);
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_recognize_encoded(
    lw_ppocr_handle handle,
    const uint8_t* encoded_image,
    uint64_t encoded_size,
    char** result_json_utf8,
    uint64_t* result_json_length) {
    const uint8_t* images[] = {encoded_image};
    const uint64_t sizes[] = {encoded_size};
    return lw_ppocr_recognize_batch_encoded(handle, images, sizes, 1,
        result_json_utf8, result_json_length);
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_recognize_batch_encoded(
    lw_ppocr_handle handle,
    const uint8_t* const* encoded_images,
    const uint64_t* encoded_sizes,
    uint64_t image_count,
    char** result_json_utf8,
    uint64_t* result_json_length) {
    if (handle == nullptr || encoded_images == nullptr || encoded_sizes == nullptr ||
        image_count == 0 || image_count > 256 ||
        !ValidOutput(result_json_utf8, result_json_length)) {
        g_last_error = "recognition arguments are invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    return Guard([&]() {
        const auto decode_start = Clock::now();
        std::vector<cv::Mat> images;
        images.reserve(static_cast<size_t>(image_count));
        for (uint64_t index = 0; index < image_count; ++index) {
            images.push_back(DecodeImage(encoded_images[index],
                encoded_sizes[index], handle->max_image_pixels));
        }
        const double decode_ms = Milliseconds(decode_start, Clock::now());
        AllocateJson(RecognitionJson(handle->engine->RecognizeBatch(images),
            decode_ms), result_json_utf8, result_json_length);
    }, LW_PPOCR_STATUS_INFERENCE_ERROR);
}

void LW_PPOCR_CALL lw_ppocr_string_free(char* value) {
    std::free(value);
}

uint64_t LW_PPOCR_CALL lw_ppocr_get_last_error(
    lw_ppocr_handle,
    char* buffer_utf8,
    uint64_t buffer_capacity) {
    return CopyText(g_last_error, buffer_utf8, buffer_capacity);
}

void LW_PPOCR_CALL lw_ppocr_destroy(lw_ppocr_handle* handle) {
    if (handle == nullptr || *handle == nullptr) {
        return;
    }
    delete *handle;
    *handle = nullptr;
    g_last_error.clear();
}
