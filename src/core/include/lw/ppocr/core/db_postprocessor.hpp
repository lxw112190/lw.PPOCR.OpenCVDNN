#pragma once

#include <opencv2/core.hpp>

#include <array>
#include <string>
#include <vector>

namespace lw::ppocr::core {

class DbPostprocessor {
public:
    std::vector<std::array<cv::Point2f, 4>> Run(
        const cv::Mat& prediction,
        const cv::Mat& bitmap,
        float box_threshold,
        float unclip_ratio,
        const std::string& score_mode,
        float ratio_h,
        float ratio_w,
        const cv::Size& original_size) const;

private:
    static std::array<cv::Point2f, 4> MiniBox(
        const cv::RotatedRect& rectangle,
        float& shortest_side);
    static float BoxScoreFast(
        const std::array<cv::Point2f, 4>& box,
        const cv::Mat& prediction);
    static float PolygonScore(
        const std::vector<cv::Point>& contour,
        const cv::Mat& prediction);
    static cv::RotatedRect Unclip(
        const std::array<cv::Point2f, 4>& box,
        float ratio);
};

}  // namespace lw::ppocr::core

