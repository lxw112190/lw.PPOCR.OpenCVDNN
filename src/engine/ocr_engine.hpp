#pragma once

#include <lw/ppocr/core/ocr_types.hpp>
#include <lw/ppocr.h>

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lw::ppocr::opencv_dnn {

class OcrEngine {
public:
    explicit OcrEngine(const lw_ppocr_config& config);
    ~OcrEngine();

    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    core::PipelineResult Run(const cv::Mat& image);
    core::RecognitionResult RecognizeBatch(
        const std::vector<cv::Mat>& images);
    void Log(lw_ppocr_log_level level, const std::string& message) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lw::ppocr::opencv_dnn
