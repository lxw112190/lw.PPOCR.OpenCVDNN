#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <string>
#include <vector>

namespace lw::ppocr::core {

struct StageTiming {
    double preprocess_ms = 0.0;
    double inference_ms = 0.0;
    double postprocess_ms = 0.0;
    double total_ms = 0.0;
};

struct OcrRegion {
    std::array<cv::Point2f, 4> box{};
    std::string text;
    float score = 0.0f;
    int32_t cls_label = -1;
    float cls_score = 0.0f;
};

struct PipelineResult {
    std::vector<OcrRegion> regions;
    StageTiming detector;
    StageTiming classifier;
    StageTiming recognizer;
    StageTiming pipeline;
};

struct RecognitionResult {
    std::vector<OcrRegion> items;
    StageTiming classifier;
    StageTiming recognizer;
    StageTiming pipeline;
};

}  // namespace lw::ppocr::core
