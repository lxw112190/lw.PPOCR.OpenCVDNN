#include <lw/ppocr/core/db_postprocessor.hpp>

#include <clipper.h>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace lw::ppocr::core {
namespace {

template <typename T>
T Clamp(T value, T minimum, T maximum) {
    return std::max(minimum, std::min(value, maximum));
}

std::array<cv::Point2f, 4> OrderClockwise(std::array<cv::Point2f, 4> points) {
    std::sort(points.begin(), points.end(),
        [](const cv::Point2f& left, const cv::Point2f& right) {
            return left.x < right.x;
        });

    const cv::Point2f left_top = points[0].y < points[1].y ? points[0] : points[1];
    const cv::Point2f left_bottom = points[0].y < points[1].y ? points[1] : points[0];
    const cv::Point2f right_top = points[2].y < points[3].y ? points[2] : points[3];
    const cv::Point2f right_bottom = points[2].y < points[3].y ? points[3] : points[2];
    return {left_top, right_top, right_bottom, left_bottom};
}

}  // namespace

std::array<cv::Point2f, 4> DbPostprocessor::MiniBox(
    const cv::RotatedRect& rectangle,
    float& shortest_side) {
    std::array<cv::Point2f, 4> points{};
    rectangle.points(points.data());
    shortest_side = std::min(rectangle.size.width, rectangle.size.height);
    return OrderClockwise(points);
}

float DbPostprocessor::BoxScoreFast(
    const std::array<cv::Point2f, 4>& box,
    const cv::Mat& prediction) {
    float min_x = box[0].x;
    float max_x = box[0].x;
    float min_y = box[0].y;
    float max_y = box[0].y;
    for (const cv::Point2f& point : box) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    const int left = Clamp(static_cast<int>(std::floor(min_x)), 0, prediction.cols - 1);
    const int right = Clamp(static_cast<int>(std::ceil(max_x)), 0, prediction.cols - 1);
    const int top = Clamp(static_cast<int>(std::floor(min_y)), 0, prediction.rows - 1);
    const int bottom = Clamp(static_cast<int>(std::ceil(max_y)), 0, prediction.rows - 1);
    if (right < left || bottom < top) {
        return 0.0f;
    }

    cv::Mat mask = cv::Mat::zeros(bottom - top + 1, right - left + 1, CV_8UC1);
    std::array<cv::Point, 4> local{};
    for (size_t index = 0; index < box.size(); ++index) {
        local[index] = cv::Point(
            static_cast<int>(box[index].x) - left,
            static_cast<int>(box[index].y) - top);
    }
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{
        std::vector<cv::Point>(local.begin(), local.end())}, cv::Scalar(1));
    return static_cast<float>(cv::mean(
        prediction(cv::Rect(left, top, right - left + 1, bottom - top + 1)),
        mask)[0]);
}

float DbPostprocessor::PolygonScore(
    const std::vector<cv::Point>& contour,
    const cv::Mat& prediction) {
    const cv::Rect bounds = cv::boundingRect(contour) &
        cv::Rect(0, 0, prediction.cols, prediction.rows);
    if (bounds.empty()) {
        return 0.0f;
    }

    std::vector<cv::Point> local = contour;
    for (cv::Point& point : local) {
        point.x -= bounds.x;
        point.y -= bounds.y;
    }
    cv::Mat mask = cv::Mat::zeros(bounds.height, bounds.width, CV_8UC1);
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{local}, cv::Scalar(1));
    return static_cast<float>(cv::mean(prediction(bounds), mask)[0]);
}

cv::RotatedRect DbPostprocessor::Unclip(
    const std::array<cv::Point2f, 4>& box,
    float ratio) {
    double area_twice = 0.0;
    double perimeter = 0.0;
    for (size_t index = 0; index < box.size(); ++index) {
        const cv::Point2f& current = box[index];
        const cv::Point2f& next = box[(index + 1) % box.size()];
        area_twice += current.x * next.y - current.y * next.x;
        perimeter += cv::norm(current - next);
    }
    const double area = std::abs(area_twice) * 0.5;
    if (perimeter <= 0.0) {
        return {};
    }

    ClipperLib::Path path;
    for (const cv::Point2f& point : box) {
        path.emplace_back(
            static_cast<ClipperLib::cInt>(std::llround(point.x)),
            static_cast<ClipperLib::cInt>(std::llround(point.y)));
    }
    ClipperLib::ClipperOffset offset;
    offset.AddPath(path, ClipperLib::jtRound, ClipperLib::etClosedPolygon);
    ClipperLib::Paths expanded;
    offset.Execute(expanded, area * ratio / perimeter);

    std::vector<cv::Point2f> points;
    for (const ClipperLib::Path& polygon : expanded) {
        for (const ClipperLib::IntPoint& point : polygon) {
            points.emplace_back(
                static_cast<float>(point.X), static_cast<float>(point.Y));
        }
    }
    return points.empty() ? cv::RotatedRect() : cv::minAreaRect(points);
}

std::vector<std::array<cv::Point2f, 4>> DbPostprocessor::Run(
    const cv::Mat& prediction,
    const cv::Mat& bitmap,
    float box_threshold,
    float unclip_ratio,
    const std::string& score_mode,
    float ratio_h,
    float ratio_w,
    const cv::Size& original_size) const {
    constexpr int minimum_side = 3;
    constexpr size_t maximum_candidates = 1000;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    std::vector<std::array<cv::Point2f, 4>> boxes;
    const size_t candidate_count = std::min(contours.size(), maximum_candidates);
    for (size_t index = 0; index < candidate_count; ++index) {
        if (contours[index].size() <= 2) {
            continue;
        }

        float shortest_side = 0.0f;
        const auto mini_box = MiniBox(cv::minAreaRect(contours[index]), shortest_side);
        if (shortest_side < minimum_side) {
            continue;
        }

        const float score = score_mode == "slow"
            ? PolygonScore(contours[index], prediction)
            : BoxScoreFast(mini_box, prediction);
        if (score < box_threshold) {
            continue;
        }

        const cv::RotatedRect expanded = Unclip(mini_box, unclip_ratio);
        if (expanded.size.width < 1.001f && expanded.size.height < 1.001f) {
            continue;
        }

        auto box = MiniBox(expanded, shortest_side);
        if (shortest_side < minimum_side + 2) {
            continue;
        }

        for (cv::Point2f& point : box) {
            point.x = Clamp(point.x / ratio_w, 0.0f,
                static_cast<float>(original_size.width - 1));
            point.y = Clamp(point.y / ratio_h, 0.0f,
                static_cast<float>(original_size.height - 1));
        }
        box = OrderClockwise(box);
        if (cv::norm(box[0] - box[1]) <= 4.0 ||
            cv::norm(box[0] - box[3]) <= 4.0) {
            continue;
        }
        boxes.push_back(box);
    }
    return boxes;
}

}  // namespace lw::ppocr::core
