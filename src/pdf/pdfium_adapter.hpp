#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lw::ppocr::pdf {

enum class Mode : uint32_t {
    Auto = 0,
    Text = 1,
    Ocr = 2,
    Hybrid = 3,
};

struct Options {
    uint32_t dpi = 200;
    uint32_t first_page = 0;
    uint32_t page_count = 0;
    uint32_t max_pages = 10;
    uint64_t max_page_pixels = 25000000;
    uint64_t max_total_pixels = 100000000;
    Mode mode = Mode::Auto;
};

struct TextItem {
    std::string text;
    std::vector<cv::Point2f> box;
    bool visible = true;
};

struct Page {
    uint32_t page_index = 0;
    int width = 0;
    int height = 0;
    cv::Mat image;
    std::vector<TextItem> text;
    bool text_layer_usable = false;
    bool has_large_image = false;
    double text_extract_ms = 0.0;
    double render_ms = 0.0;
};

class Document final {
public:
    Document(const uint8_t* data, uint64_t size, const Options& options);
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    uint32_t page_count() const noexcept;
    Page process_page(uint32_t page_index, bool extract_text, bool render);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

bool IsAvailable();

}  // namespace lw::ppocr::pdf
