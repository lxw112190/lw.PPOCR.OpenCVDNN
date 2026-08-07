#include <lw/ppocr.h>

#include "../engine/ocr_engine.hpp"
#include "../pdf/pdfium_adapter.hpp"

#include <json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using PdfMode = lw::ppocr::pdf::Mode;

struct lw_ppocr_engine {
    std::unique_ptr<lw::ppocr::opencv_dnn::OcrEngine> engine;
    uint64_t max_image_pixels = 40000000;
    uint32_t max_batch_images = 32;
    uint64_t max_batch_total_pixels = 40000000;
    uint64_t max_batch_decoded_bytes = 120000000;
};

namespace {

thread_local std::string g_last_error;

constexpr uint32_t kDefaultMaxBatchImages = 32;
constexpr uint64_t kDefaultMaxBatchTotalPixels = 40000000;
constexpr uint64_t kDefaultMaxBatchDecodedBytes = 120000000;
constexpr size_t kBatchChunkSize = 8;

class LimitExceeded final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DocumentError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

bool HasKnownImageSignature(const uint8_t* data, uint64_t size) {
    if (data == nullptr) return false;
    const bool jpeg = size >= 3 && data[0] == 0xFF && data[1] == 0xD8 &&
        data[2] == 0xFF;
    const bool png = size >= 8 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47 && data[4] == 0x0D &&
        data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
    const bool bmp = size >= 2 && data[0] == 'B' && data[1] == 'M';
    const bool webp = size >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
        std::memcmp(data + 8, "WEBP", 4) == 0;
    const bool tiff = size >= 4 &&
        ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A &&
          data[3] == 0x00) ||
         (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 &&
          data[3] == 0x2A));
    const bool gif = size >= 6 &&
        (std::memcmp(data, "GIF87a", 6) == 0 ||
         std::memcmp(data, "GIF89a", 6) == 0);
    return jpeg || png || bmp || webp || tiff || gif;
}

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
    if (!HasKnownImageSignature(encoded, encoded_size)) {
        throw std::invalid_argument("unsupported or corrupt encoded image");
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
            {"cls_score", region.cls_score}
        };
        for (size_t index = 0; index < region.box.size(); ++index) {
            item["x" + std::to_string(index + 1)] = region.box[index].x;
            item["y" + std::to_string(index + 1)] = region.box[index].y;
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

PdfMode PdfModeFrom(uint32_t value) {
    switch (value) {
    case LW_PPOCR_PDF_MODE_TEXT: return PdfMode::Text;
    case LW_PPOCR_PDF_MODE_OCR: return PdfMode::Ocr;
    case LW_PPOCR_PDF_MODE_HYBRID: return PdfMode::Hybrid;
    case LW_PPOCR_PDF_MODE_AUTO: return PdfMode::Auto;
    default: throw std::invalid_argument("unsupported PDF processing mode");
    }
}

lw::ppocr::pdf::Options PdfOptionsFrom(const lw_ppocr_pdf_options& value) {
    if (value.struct_size < sizeof(value) ||
        value.api_version != LW_PPOCR_API_VERSION) {
        throw std::invalid_argument("PDF options structure is invalid");
    }
    lw::ppocr::pdf::Options options;
    options.dpi = value.dpi;
    options.first_page = value.first_page;
    options.page_count = value.page_count;
    options.max_pages = value.max_pages;
    options.max_page_pixels = value.max_page_pixels;
    options.max_total_pixels = value.max_total_pixels;
    options.mode = PdfModeFrom(value.mode);
    if (options.dpi == 0) options.dpi = 200;
    if (options.max_pages == 0) options.max_pages = 10;
    if (options.max_page_pixels == 0) options.max_page_pixels = 25000000;
    if (options.max_total_pixels == 0) options.max_total_pixels = 100000000;
    return options;
}

json PdfTextItemJson(const lw::ppocr::pdf::TextItem& item) {
    json value = {
        {"text", item.text},
        {"source", "pdf_text"},
        {"score", nullptr}
    };
    for (size_t index = 0; index < item.box.size() && index < 4; ++index) {
        value["x" + std::to_string(index + 1)] = item.box[index].x;
        value["y" + std::to_string(index + 1)] = item.box[index].y;
    }
    return value;
}

double BoxIoU(const json& left, const lw::ppocr::pdf::TextItem& right) {
    if (!left.contains("x1") || !left.contains("y1") ||
        right.box.size() < 4) return 0.0;
    double left_x = left.at("x1").get<double>();
    double top_y = left.at("y1").get<double>();
    double right_x = left_x;
    double bottom_y = top_y;
    for (int index = 2; index <= 4; ++index) {
        right_x = std::max(right_x, left.at(
            "x" + std::to_string(index)).get<double>());
        bottom_y = std::max(bottom_y, left.at(
            "y" + std::to_string(index)).get<double>());
        left_x = std::min(left_x, left.at(
            "x" + std::to_string(index)).get<double>());
        top_y = std::min(top_y, left.at(
            "y" + std::to_string(index)).get<double>());
    }
    float pdf_left = right.box[0].x;
    float pdf_top = right.box[0].y;
    float pdf_right = pdf_left;
    float pdf_bottom = pdf_top;
    for (const auto& point : right.box) {
        pdf_left = std::min(pdf_left, point.x);
        pdf_top = std::min(pdf_top, point.y);
        pdf_right = std::max(pdf_right, point.x);
        pdf_bottom = std::max(pdf_bottom, point.y);
    }
    const double intersection_left = std::max(left_x, static_cast<double>(pdf_left));
    const double intersection_top = std::max(top_y, static_cast<double>(pdf_top));
    const double intersection_right = std::min(right_x, static_cast<double>(pdf_right));
    const double intersection_bottom = std::min(bottom_y, static_cast<double>(pdf_bottom));
    if (intersection_right <= intersection_left ||
        intersection_bottom <= intersection_top) return 0.0;
    const double intersection = (intersection_right - intersection_left) *
        (intersection_bottom - intersection_top);
    const double union_area = (right_x - left_x) * (bottom_y - top_y) +
        (pdf_right - pdf_left) * (pdf_bottom - pdf_top) - intersection;
    return union_area <= 0.0 ? 0.0 : intersection / union_area;
}

std::string CompactText(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(),
        [](unsigned char character) { return std::isspace(character) != 0; }),
        value.end());
    return value;
}

json PdfPageJson(const lw::ppocr::pdf::Page& page,
                 const json& ocr,
                 const std::string& method) {
    json items = json::array();
    if (method == "pdf_text") {
        for (const auto& item : page.text) {
            if (item.visible) items.push_back(PdfTextItemJson(item));
        }
    } else {
        if (ocr.contains("result") && ocr.at("result").is_array()) {
            for (const auto& original : ocr.at("result")) {
                json item = original;
                item["source"] = "ocr";
                items.push_back(std::move(item));
            }
        }
        if (method == "hybrid") {
            constexpr double kHybridDuplicateIoU = 0.45;
            // Match each PDF text item to at most one OCR result.  A greedy
            // first-match can reuse a single OCR box for repeated text and
            // make the result count depend on PDF object order, so choose
            // the highest-overlap candidate deterministically instead.
            std::vector<bool> ocr_matched(items.size(), false);
            for (const auto& text : page.text) {
                if (!text.visible || text.text.empty()) continue;
                const std::string right = CompactText(text.text);
                std::size_t best_index = items.size();
                double best_iou = kHybridDuplicateIoU;
                for (std::size_t index = 0; index < items.size(); ++index) {
                    if (index >= ocr_matched.size() || ocr_matched[index]) {
                        continue;
                    }
                    auto& item = items[index];
                    const std::string left = CompactText(
                        item.value("text", std::string{}));
                    const double iou = BoxIoU(item, text);
                    if (!left.empty() && left == right && iou >= best_iou) {
                        best_iou = iou;
                        best_index = index;
                    }
                }
                if (best_index < items.size()) {
                    items[best_index]["source"] = "hybrid";
                    ocr_matched[best_index] = true;
                } else {
                    items.push_back(PdfTextItemJson(text));
                }
            }
        }
    }
    json result = {
        {"page_index", page.page_index},
        {"image", {{"width", page.width}, {"height", page.height}}},
        {"method", method},
        {"text_layer_usable", page.text_layer_usable},
        {"result", std::move(items)},
        {"timing_ms", {
            {"text_extract", page.text_extract_ms},
            {"render", page.render_ms}
        }}
    };
    if (ocr.contains("timing")) result["ocr_timing"] = ocr.at("timing");
    return result;
}

void AccumulateTiming(
    lw::ppocr::core::StageTiming& destination,
    const lw::ppocr::core::StageTiming& source) {
    destination.preprocess_ms += source.preprocess_ms;
    destination.inference_ms += source.inference_ms;
    destination.postprocess_ms += source.postprocess_ms;
    destination.total_ms += source.total_ms;
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
    } catch (const LimitExceeded& exception) {
        g_last_error = exception.what();
        return LW_PPOCR_STATUS_LIMIT_EXCEEDED;
    } catch (const DocumentError& exception) {
        g_last_error = exception.what();
        return LW_PPOCR_STATUS_DOCUMENT_ERROR;
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
    config->max_batch_images = kDefaultMaxBatchImages;
    config->max_batch_total_pixels = kDefaultMaxBatchTotalPixels;
    config->max_batch_decoded_bytes = kDefaultMaxBatchDecodedBytes;
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
    const uint32_t max_batch_images = config->max_batch_images == 0
        ? kDefaultMaxBatchImages : config->max_batch_images;
    const uint64_t max_batch_total_pixels = config->max_batch_total_pixels == 0
        ? kDefaultMaxBatchTotalPixels : config->max_batch_total_pixels;
    const uint64_t max_batch_decoded_bytes =
        config->max_batch_decoded_bytes == 0
        ? kDefaultMaxBatchDecodedBytes : config->max_batch_decoded_bytes;
    if (config->limit_side_len < 32 || config->max_image_pixels == 0 ||
        config->cls_batch_size < 1 || config->rec_batch_size < 1 ||
        config->rec_concurrency < 1 || max_batch_images < 1 ||
        max_batch_images > 256 || max_batch_total_pixels < 1 ||
        max_batch_decoded_bytes < 1) {
        g_last_error = "OCR numeric configuration is invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    return Guard([&]() {
        auto owned = std::make_unique<lw_ppocr_engine>();
        owned->max_image_pixels = config->max_image_pixels;
        owned->max_batch_images = max_batch_images;
        owned->max_batch_total_pixels = max_batch_total_pixels;
        owned->max_batch_decoded_bytes = max_batch_decoded_bytes;
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

void LW_PPOCR_CALL lw_ppocr_pdf_options_init(lw_ppocr_pdf_options* options) {
    if (options == nullptr) return;
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->api_version = LW_PPOCR_API_VERSION;
    options->mode = LW_PPOCR_PDF_MODE_AUTO;
    options->dpi = 200;
    options->max_pages = 10;
    options->max_page_pixels = 25000000;
    options->max_total_pixels = 100000000;
}

int LW_PPOCR_CALL lw_ppocr_pdfium_is_available(void) {
    return lw::ppocr::pdf::IsAvailable() ? 1 : 0;
}

lw_ppocr_status LW_PPOCR_CALL lw_ppocr_ocr_pdf_encoded(
    lw_ppocr_handle handle,
    const uint8_t* pdf_data,
    uint64_t pdf_size,
    const lw_ppocr_pdf_options* options,
    char** result_json_utf8,
    uint64_t* result_json_length) {
    if (handle == nullptr || pdf_data == nullptr || pdf_size == 0 ||
        !ValidOutput(result_json_utf8, result_json_length)) {
        g_last_error = "PDF OCR arguments are invalid";
        return LW_PPOCR_STATUS_INVALID_ARGUMENT;
    }
    return Guard([&]() {
        try {
            lw_ppocr_pdf_options defaults{};
            lw_ppocr_pdf_options_init(&defaults);
            const lw_ppocr_pdf_options& value = options == nullptr
                ? defaults : *options;
            const auto pdf_options = PdfOptionsFrom(value);
            lw::ppocr::pdf::Document document(pdf_data, pdf_size, pdf_options);
            const uint32_t total_pages = document.page_count();
            if (pdf_options.first_page >= total_pages) {
                throw std::invalid_argument("PDF first_page is out of range");
            }
            const uint32_t available = total_pages - pdf_options.first_page;
            const uint32_t requested = pdf_options.page_count == 0
                ? available : std::min(pdf_options.page_count, available);
            if (requested == 0 || requested > pdf_options.max_pages) {
                throw LimitExceeded("PDF exceeds max_pages");
            }

            json pages = json::array();
            uint64_t total_pixels = 0;
            const auto overall_start = Clock::now();
            for (uint32_t offset = 0; offset < requested; ++offset) {
                const uint32_t page_index = pdf_options.first_page + offset;
                const bool initial_extract = pdf_options.mode != PdfMode::Ocr;
                const bool initial_render = pdf_options.mode == PdfMode::Ocr ||
                    pdf_options.mode == PdfMode::Hybrid;
                auto page = document.process_page(page_index, initial_extract,
                    initial_render);
                std::string method;
                if (pdf_options.mode == PdfMode::Text) {
                    method = "pdf_text";
                } else if (pdf_options.mode == PdfMode::Ocr) {
                    method = "ocr";
                } else if (pdf_options.mode == PdfMode::Hybrid) {
                    method = "hybrid";
                } else if (page.text_layer_usable && !page.has_large_image) {
                    method = "pdf_text";
                } else if (page.text_layer_usable) {
                    method = "hybrid";
                } else {
                    method = "ocr";
                }

                if (method != "pdf_text" && page.image.empty()) {
                    // Auto mode already extracted the text layer to classify
                    // the page. Render the same page without extracting it a
                    // second time, then retain the classification metadata
                    // for hybrid output and diagnostics.
                    auto rendered = document.process_page(page_index, false, true);
                    rendered.text = std::move(page.text);
                    rendered.text_layer_usable = page.text_layer_usable;
                    rendered.has_large_image = page.has_large_image;
                    rendered.text_extract_ms = page.text_extract_ms;
                    page = std::move(rendered);
                }
                const uint64_t page_pixels = static_cast<uint64_t>(page.width) *
                    static_cast<uint64_t>(page.height);
                if (page_pixels > pdf_options.max_page_pixels ||
                    page_pixels > pdf_options.max_total_pixels - total_pixels) {
                    throw LimitExceeded("PDF exceeds cumulative pixel limits");
                }
                total_pixels += page_pixels;

                json ocr = json::object();
                if (!page.image.empty()) {
                    ocr = OcrJson(handle->engine->Run(page.image), page.image,
                        0.0);
                }
                pages.push_back(PdfPageJson(page, ocr, method));
            }
            AllocateJson({
                {"document", {
                    {"format", "pdf"},
                    {"page_count", total_pages},
                    {"processed_pages", requested},
                    {"dpi", pdf_options.dpi}
                }},
                {"pages", std::move(pages)},
                {"timing_ms", {
                    {"server_total", Milliseconds(overall_start, Clock::now())}
                }}
            }, result_json_utf8, result_json_length);
        } catch (const LimitExceeded&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& exception) {
            throw DocumentError(exception.what());
        }
    }, LW_PPOCR_STATUS_DOCUMENT_ERROR);
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
    if (image_count > handle->max_batch_images) {
        g_last_error = "batch exceeds max_batch_images";
        return LW_PPOCR_STATUS_LIMIT_EXCEEDED;
    }
    return Guard([&]() {
        uint64_t total_pixels = 0;
        uint64_t total_decoded_bytes = 0;
        double decode_ms = 0.0;
        lw::ppocr::core::RecognitionResult combined;
        combined.items.reserve(static_cast<size_t>(image_count));

        for (uint64_t begin = 0; begin < image_count;
             begin += kBatchChunkSize) {
            const uint64_t end = (std::min)(
                image_count, begin + static_cast<uint64_t>(kBatchChunkSize));
            std::vector<cv::Mat> images;
            images.reserve(static_cast<size_t>(end - begin));
            const auto decode_start = Clock::now();
            for (uint64_t index = begin; index < end; ++index) {
                cv::Mat image = DecodeImage(encoded_images[index],
                    encoded_sizes[index], handle->max_image_pixels);
                const uint64_t pixels = static_cast<uint64_t>(image.total());
                const uint64_t bytes = pixels * image.elemSize();
                if (pixels > handle->max_batch_total_pixels - total_pixels ||
                    bytes > handle->max_batch_decoded_bytes -
                        total_decoded_bytes) {
                    throw LimitExceeded(
                        "batch exceeds cumulative decoded image limits");
                }
                total_pixels += pixels;
                total_decoded_bytes += bytes;
                images.push_back(std::move(image));
            }
            decode_ms += Milliseconds(decode_start, Clock::now());

            auto partial = handle->engine->RecognizeBatch(std::move(images));
            combined.items.insert(combined.items.end(),
                std::make_move_iterator(partial.items.begin()),
                std::make_move_iterator(partial.items.end()));
            AccumulateTiming(combined.classifier, partial.classifier);
            AccumulateTiming(combined.recognizer, partial.recognizer);
            AccumulateTiming(combined.pipeline, partial.pipeline);
        }
        AllocateJson(RecognitionJson(combined, decode_ms),
            result_json_utf8, result_json_length);
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
