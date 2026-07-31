#pragma once

#include <lw/ppocr/core/ocr_types.hpp>

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace lw::ppocr::core {

cv::Mat ResizeForDetection(
    const cv::Mat& image,
    int limit_side_len,
    float& ratio_h,
    float& ratio_w);

cv::Mat ResizeForRecognition(
    const cv::Mat& image,
    int target_height,
    int target_width);

cv::Mat ResizeForClassification(
    const cv::Mat& image,
    int target_height,
    int target_width);

void NormalizePermuteBgr(
    const cv::Mat& image,
    float* destination,
    const float mean[3],
    const float scale[3]);

void NormalizePermuteBatchBgr(
    const std::vector<cv::Mat>& images,
    float* destination,
    const float mean[3],
    const float scale[3]);

cv::Mat GetRotateCropImage(
    const cv::Mat& image,
    const std::array<cv::Point2f, 4>& box);

void SortReadingOrder(std::vector<OcrRegion>& regions);
std::vector<std::string> ReadDictionary(const std::string& path_utf8);

}  // namespace lw::ppocr::core
