#include <lw/ppocr/core/image_ops.hpp>

#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace lw::ppocr::core {

cv::Mat ResizeForDetection(
    const cv::Mat& image,
    int limit_side_len,
    float& ratio_h,
    float& ratio_w) {
    const int width = image.cols;
    const int height = image.rows;
    const int max_side = std::max(width, height);
    const float ratio = max_side > limit_side_len
        ? static_cast<float>(limit_side_len) / static_cast<float>(max_side)
        : 1.0f;

    const int resized_height = std::max(
        static_cast<int>(std::round(height * ratio / 32.0f)) * 32, 32);
    const int resized_width = std::max(
        static_cast<int>(std::round(width * ratio / 32.0f)) * 32, 32);

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_width, resized_height));
    ratio_h = static_cast<float>(resized_height) / static_cast<float>(height);
    ratio_w = static_cast<float>(resized_width) / static_cast<float>(width);
    return resized;
}

cv::Mat ResizeForRecognition(
    const cv::Mat& image,
    int target_height,
    int target_width) {
    const float ratio = static_cast<float>(image.cols) /
        static_cast<float>(std::max(image.rows, 1));
    const int resized_width = std::min(
        target_width,
        std::max(1, static_cast<int>(std::ceil(target_height * ratio))));

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_width, target_height),
        0.0, 0.0, cv::INTER_LINEAR);
    if (resized_width < target_width) {
        cv::copyMakeBorder(resized, resized, 0, 0, 0,
            target_width - resized_width, cv::BORDER_CONSTANT,
            cv::Scalar(128, 128, 128));
    }
    return resized;
}

cv::Mat ResizeForClassification(
    const cv::Mat& image,
    int target_height,
    int target_width) {
    const float ratio = static_cast<float>(image.cols) /
        static_cast<float>(std::max(image.rows, 1));
    const int resized_width = std::min(
        target_width,
        std::max(1, static_cast<int>(std::ceil(target_height * ratio))));

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_width, target_height),
        0.0, 0.0, cv::INTER_LINEAR);
    if (resized_width < target_width) {
        cv::copyMakeBorder(resized, resized, 0, 0, 0,
            target_width - resized_width, cv::BORDER_CONSTANT,
            cv::Scalar(0, 0, 0));
    }
    return resized;
}

void NormalizePermuteBgr(
    const cv::Mat& image,
    float* destination,
    const float mean[3],
    const float scale[3]) {
    const int height = image.rows;
    const int width = image.cols;
    const size_t plane = static_cast<size_t>(height) * width;
    constexpr float inverse_255 = 1.0f / 255.0f;

    for (int y = 0; y < height; ++y) {
        const cv::Vec3b* row = image.ptr<cv::Vec3b>(y);
        for (int x = 0; x < width; ++x) {
            const size_t offset = static_cast<size_t>(y) * width + x;
            for (int channel = 0; channel < 3; ++channel) {
                const float value = row[x][channel] * inverse_255;
                destination[static_cast<size_t>(channel) * plane + offset] =
                    (value - mean[channel]) * scale[channel];
            }
        }
    }
}

void NormalizePermuteBatchBgr(
    const std::vector<cv::Mat>& images,
    float* destination,
    const float mean[3],
    const float scale[3]) {
    if (images.empty()) {
        return;
    }
    const size_t image_elements =
        static_cast<size_t>(images.front().rows) * images.front().cols * 3;
    for (size_t index = 0; index < images.size(); ++index) {
        NormalizePermuteBgr(images[index],
            destination + index * image_elements, mean, scale);
    }
}

cv::Mat GetRotateCropImage(
    const cv::Mat& image,
    const std::array<cv::Point2f, 4>& box) {
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

    const int left = std::max(0, static_cast<int>(std::floor(min_x)));
    const int top = std::max(0, static_cast<int>(std::floor(min_y)));
    const int right = std::min(image.cols, static_cast<int>(std::ceil(max_x)));
    const int bottom = std::min(image.rows, static_cast<int>(std::ceil(max_y)));
    if (right <= left || bottom <= top) {
        return {};
    }

    const cv::Rect roi(left, top, right - left, bottom - top);
    const cv::Mat cropped = image(roi);
    std::array<cv::Point2f, 4> local = box;
    for (cv::Point2f& point : local) {
        point.x -= static_cast<float>(left);
        point.y -= static_cast<float>(top);
    }

    const int target_width = std::max(1, static_cast<int>(std::round(
        cv::norm(local[0] - local[1]))));
    const int target_height = std::max(1, static_cast<int>(std::round(
        cv::norm(local[0] - local[3]))));
    const std::array<cv::Point2f, 4> destination = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(target_width), 0.0f),
        cv::Point2f(static_cast<float>(target_width), static_cast<float>(target_height)),
        cv::Point2f(0.0f, static_cast<float>(target_height))
    };

    const cv::Mat transform = cv::getPerspectiveTransform(local.data(), destination.data());
    cv::Mat result;
    cv::warpPerspective(cropped, result, transform,
        cv::Size(target_width, target_height), cv::INTER_LINEAR,
        cv::BORDER_REPLICATE);
    if (result.rows >= result.cols * 1.5f) {
        cv::rotate(result, result, cv::ROTATE_90_CLOCKWISE);
    }
    return result;
}

void SortReadingOrder(std::vector<OcrRegion>& regions) {
    std::stable_sort(regions.begin(), regions.end(),
        [](const OcrRegion& left, const OcrRegion& right) {
            const float delta_y = left.box[0].y - right.box[0].y;
            if (std::abs(delta_y) < 10.0f) {
                return left.box[0].x < right.box[0].x;
            }
            return delta_y < 0.0f;
        });
}

std::vector<std::string> ReadDictionary(const std::string& path_utf8) {
    std::ifstream input(path_utf8, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open recognition dictionary: " + path_utf8);
    }

    std::vector<std::string> labels;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        labels.push_back(line);
    }
    if (labels.empty()) {
        throw std::runtime_error("recognition dictionary is empty: " + path_utf8);
    }
    labels.insert(labels.begin(), "#");
    labels.emplace_back(" ");
    return labels;
}

}  // namespace lw::ppocr::core
