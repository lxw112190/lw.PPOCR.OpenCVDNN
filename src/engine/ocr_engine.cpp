#include "ocr_engine.hpp"

#include <lw/ppocr/core/db_postprocessor.hpp>
#include <lw/ppocr/core/image_ops.hpp>
#include <lw/ppocr/core/model_manifest.hpp>

#include <opencv2/dnn.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lw::ppocr::opencv_dnn {
namespace {

using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void ConfigureCpuBackend(cv::dnn::Net& net) {
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
#if CV_VERSION_MAJOR < 5
    // OpenCV 5's new graph engine currently runs on CPU by default and emits
    // a warning when a target is selected explicitly.
    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
#endif
}

cv::dnn::Net LoadOnnxNet(const std::filesystem::path& path) {
    const std::vector<unsigned char> bytes = core::ReadBinaryFile(path);
    cv::dnn::Net net = cv::dnn::readNetFromONNX(bytes);
    ConfigureCpuBackend(net);
    return net;
}

class Detector {
public:
    Detector(const core::ModelStage& model, const lw_ppocr_config& config)
        : net_(LoadOnnxNet(model.path)),
          limit_side_len_(config.limit_side_len),
          threshold_(config.det_db_threshold),
          box_threshold_(config.det_db_box_threshold),
          unclip_ratio_(config.det_db_unclip_ratio),
          use_dilation_(config.det_use_dilation != 0) {
        output_names_ = net_.getUnconnectedOutLayersNames();
        if (output_names_.empty()) {
            throw std::runtime_error("detector model has no output");
        }
    }

    std::vector<core::OcrRegion> Run(
        const cv::Mat& image,
        core::StageTiming& timing) {
        const Clock::time_point total_start = Clock::now();
        const Clock::time_point preprocess_start = Clock::now();
        float ratio_h = 1.0f;
        float ratio_w = 1.0f;
        cv::Mat resized = core::ResizeForDetection(
            image, limit_side_len_, ratio_h, ratio_w);

        const size_t element_count =
            static_cast<size_t>(resized.rows) * resized.cols * 3;
        input_.resize(element_count);
        constexpr float mean[3] = {0.485f, 0.456f, 0.406f};
        constexpr float scale[3] = {
            1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f};
        core::NormalizePermuteBgr(resized, input_.data(), mean, scale);
        const Clock::time_point preprocess_end = Clock::now();

        const int dimensions[] = {1, 3, resized.rows, resized.cols};
        cv::Mat blob(4, dimensions, CV_32F, input_.data());
        const Clock::time_point inference_start = Clock::now();
        net_.setInput(blob);
        std::vector<cv::Mat> outputs;
        net_.forward(outputs, output_names_);
        const Clock::time_point inference_end = Clock::now();
        if (outputs.empty() || outputs.front().empty()) {
            throw std::runtime_error("detector returned an empty output tensor");
        }

        const Clock::time_point postprocess_start = Clock::now();
        cv::Mat output = outputs.front();
        if (!output.isContinuous()) {
            output = output.clone();
        }
        if (output.depth() != CV_32F || output.dims < 2) {
            throw std::runtime_error("detector output tensor has an unsupported layout");
        }
        const int map_height = output.dims >= 4
            ? output.size[output.dims - 2]
            : output.rows;
        const int map_width = output.dims >= 4
            ? output.size[output.dims - 1]
            : output.cols;
        cv::Mat prediction(map_height, map_width, CV_32F, output.ptr<float>());
        cv::Mat bitmap;
        cv::compare(prediction, threshold_, bitmap, cv::CMP_GT);
        if (use_dilation_) {
            cv::dilate(bitmap, bitmap,
                cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));
        }

        const auto boxes = postprocessor_.Run(
            prediction, bitmap, box_threshold_, unclip_ratio_, "slow",
            ratio_h, ratio_w, image.size());
        std::vector<core::OcrRegion> regions;
        regions.reserve(boxes.size());
        for (const auto& box : boxes) {
            core::OcrRegion region;
            region.box = box;
            regions.push_back(std::move(region));
        }
        core::SortReadingOrder(regions);
        const Clock::time_point postprocess_end = Clock::now();

        timing.preprocess_ms = Milliseconds(preprocess_start, preprocess_end);
        timing.inference_ms = Milliseconds(inference_start, inference_end);
        timing.postprocess_ms = Milliseconds(postprocess_start, postprocess_end);
        timing.total_ms = Milliseconds(total_start, postprocess_end);
        return regions;
    }

private:
    cv::dnn::Net net_;
    std::vector<cv::String> output_names_;
    std::vector<float> input_;
    core::DbPostprocessor postprocessor_;
    int limit_side_len_ = 960;
    float threshold_ = 0.3f;
    float box_threshold_ = 0.6f;
    float unclip_ratio_ = 1.6f;
    bool use_dilation_ = false;
};

class Classifier {
public:
    Classifier(const core::ModelStage& model, const lw_ppocr_config& config)
        : net_(LoadOnnxNet(model.path)),
          height_(model.input_shape[2]),
          width_(model.input_shape[3]),
          batch_size_(config.cls_batch_size),
          threshold_(config.cls_threshold) {}

    void Run(
        std::vector<cv::Mat>& images,
        std::vector<core::OcrRegion>& regions,
        core::StageTiming& timing) {
        const Clock::time_point total_start = Clock::now();
        constexpr float mean[3] = {0.5f, 0.5f, 0.5f};
        constexpr float scale[3] = {2.0f, 2.0f, 2.0f};

        for (size_t begin = 0; begin < images.size();
             begin += static_cast<size_t>(batch_size_)) {
            const size_t end = std::min(
                images.size(), begin + static_cast<size_t>(batch_size_));
            const Clock::time_point preprocess_start = Clock::now();
            std::vector<cv::Mat> resized;
            resized.reserve(end - begin);
            for (size_t index = begin; index < end; ++index) {
                resized.push_back(core::ResizeForClassification(
                    images[index], height_, width_));
            }
            input_.resize(resized.size() * 3ULL * height_ * width_);
            core::NormalizePermuteBatchBgr(resized, input_.data(), mean, scale);
            const Clock::time_point preprocess_end = Clock::now();

            const int dimensions[] = {
                static_cast<int>(resized.size()), 3, height_, width_};
            cv::Mat blob(4, dimensions, CV_32F, input_.data());
            const Clock::time_point inference_start = Clock::now();
            net_.setInput(blob);
            cv::Mat output = net_.forward();
            const Clock::time_point inference_end = Clock::now();

            const Clock::time_point postprocess_start = Clock::now();
            if (!output.isContinuous()) {
                output = output.clone();
            }
            if (output.depth() != CV_32F || output.dims < 2) {
                throw std::runtime_error(
                    "classifier output tensor has an unsupported layout");
            }
            const int output_batch = output.size[0];
            const int class_count = output.size[1];
            const float* values = output.ptr<float>();
            const int count = std::min(output_batch, static_cast<int>(resized.size()));
            for (int batch = 0; batch < count; ++batch) {
                const float* row = values + static_cast<size_t>(batch) * class_count;
                const float* maximum = std::max_element(row, row + class_count);
                const int label = static_cast<int>(maximum - row);
                const size_t index = begin + static_cast<size_t>(batch);
                regions[index].cls_label = label;
                regions[index].cls_score = *maximum;
                if ((label % 2) == 1 && *maximum > threshold_) {
                    cv::rotate(images[index], images[index], cv::ROTATE_180);
                }
            }
            const Clock::time_point postprocess_end = Clock::now();
            timing.preprocess_ms += Milliseconds(preprocess_start, preprocess_end);
            timing.inference_ms += Milliseconds(inference_start, inference_end);
            timing.postprocess_ms += Milliseconds(postprocess_start, postprocess_end);
        }
        timing.total_ms = Milliseconds(total_start, Clock::now());
    }

private:
    cv::dnn::Net net_;
    std::vector<float> input_;
    int height_ = 48;
    int width_ = 192;
    int batch_size_ = 8;
    float threshold_ = 0.9f;
};

class Recognizer {
public:
    Recognizer(const core::ModelStage& model,
        const std::filesystem::path& dictionary,
        const lw_ppocr_config& config)
        : labels_(core::ReadDictionary(core::PathToUtf8(dictionary))),
          height_(model.input_shape[2]),
          minimum_width_(model.input_shape[3]),
          batch_size_(config.rec_batch_size),
          concurrency_(config.rec_concurrency) {
        const std::vector<unsigned char> bytes = core::ReadBinaryFile(model.path);
        nets_.reserve(static_cast<size_t>(concurrency_));
        for (int index = 0; index < concurrency_; ++index) {
            cv::dnn::Net net = cv::dnn::readNetFromONNX(bytes);
            ConfigureCpuBackend(net);
            nets_.push_back(std::move(net));
        }
    }

    void Run(
        const std::vector<cv::Mat>& images,
        std::vector<core::OcrRegion>& regions,
        core::StageTiming& timing) {
        if (images.empty()) {
            return;
        }
        const Clock::time_point total_start = Clock::now();
        std::vector<size_t> sorted(images.size());
        std::iota(sorted.begin(), sorted.end(), 0);
        std::sort(sorted.begin(), sorted.end(), [&images](size_t left, size_t right) {
            return static_cast<float>(images[left].cols) / images[left].rows <
                static_cast<float>(images[right].cols) / images[right].rows;
        });

        std::vector<std::vector<size_t>> batches;
        for (size_t begin = 0; begin < sorted.size();
             begin += static_cast<size_t>(batch_size_)) {
            const size_t end = std::min(
                sorted.size(), begin + static_cast<size_t>(batch_size_));
            batches.emplace_back(sorted.begin() + begin, sorted.begin() + end);
        }

        const size_t worker_count = std::min(nets_.size(), batches.size());
        std::atomic<size_t> next_batch{0};
        std::vector<std::future<core::StageTiming>> workers;
        workers.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers.push_back(std::async(std::launch::async,
                [this, worker, &next_batch, &batches, &images, &regions]() {
                    core::StageTiming worker_timing;
                    std::vector<float> input;
                    for (;;) {
                        const size_t batch_index = next_batch.fetch_add(1);
                        if (batch_index >= batches.size()) {
                            break;
                        }
                        RunBatch(nets_[worker], batches[batch_index], images,
                            regions, input, worker_timing);
                    }
                    return worker_timing;
                }));
        }

        for (auto& worker : workers) {
            const core::StageTiming value = worker.get();
            timing.preprocess_ms = std::max(
                timing.preprocess_ms, value.preprocess_ms);
            timing.inference_ms = std::max(
                timing.inference_ms, value.inference_ms);
            timing.postprocess_ms = std::max(
                timing.postprocess_ms, value.postprocess_ms);
        }
        timing.total_ms = Milliseconds(total_start, Clock::now());
    }

private:
    void RunBatch(
        cv::dnn::Net& net,
        const std::vector<size_t>& indices,
        const std::vector<cv::Mat>& images,
        std::vector<core::OcrRegion>& regions,
        std::vector<float>& input,
        core::StageTiming& timing) const {
        const Clock::time_point preprocess_start = Clock::now();
        float maximum_ratio = static_cast<float>(minimum_width_) / height_;
        for (size_t index : indices) {
            maximum_ratio = std::max(maximum_ratio,
                static_cast<float>(images[index].cols) / images[index].rows);
        }
        const int batch_width = std::max(
            minimum_width_, static_cast<int>(std::ceil(height_ * maximum_ratio)));
        std::vector<cv::Mat> resized;
        resized.reserve(indices.size());
        for (size_t index : indices) {
            resized.push_back(core::ResizeForRecognition(
                images[index], height_, batch_width));
        }
        input.resize(resized.size() * 3ULL * height_ * batch_width);
        constexpr float mean[3] = {0.5f, 0.5f, 0.5f};
        constexpr float scale[3] = {2.0f, 2.0f, 2.0f};
        core::NormalizePermuteBatchBgr(resized, input.data(), mean, scale);
        const Clock::time_point preprocess_end = Clock::now();

        const int dimensions[] = {
            static_cast<int>(indices.size()), 3, height_, batch_width};
        cv::Mat blob(4, dimensions, CV_32F, input.data());
        const Clock::time_point inference_start = Clock::now();
        net.setInput(blob);
        cv::Mat output = net.forward();
        const Clock::time_point inference_end = Clock::now();

        const Clock::time_point postprocess_start = Clock::now();
        if (!output.isContinuous()) {
            output = output.clone();
        }
        if (output.depth() != CV_32F || output.dims < 2) {
            throw std::runtime_error("recognizer output tensor has an unsupported layout");
        }
        const int output_batch = output.dims >= 3 ? output.size[0] : 1;
        const int time_steps = output.dims >= 3 ? output.size[1] : output.size[0];
        const int class_count = output.dims >= 3 ? output.size[2] : output.size[1];
        const float* values = output.ptr<float>();
        const size_t batch_stride =
            static_cast<size_t>(time_steps) * class_count;
        const int count = std::min(output_batch, static_cast<int>(indices.size()));

        for (int batch = 0; batch < count; ++batch) {
            std::string text;
            float score_sum = 0.0f;
            int score_count = 0;
            int previous = -1;
            const float* batch_values = values + static_cast<size_t>(batch) * batch_stride;
            for (int step = 0; step < time_steps; ++step) {
                const float* row = batch_values + static_cast<size_t>(step) * class_count;
                const float* maximum = std::max_element(row, row + class_count);
                const int label = static_cast<int>(maximum - row);
                if (label > 0 && label != previous &&
                    label < static_cast<int>(labels_.size())) {
                    text += labels_[static_cast<size_t>(label)];
                    score_sum += *maximum;
                    ++score_count;
                }
                previous = label;
            }
            core::OcrRegion& region = regions[indices[static_cast<size_t>(batch)]];
            region.text = std::move(text);
            region.score = score_count > 0
                ? score_sum / static_cast<float>(score_count)
                : 0.0f;
        }
        const Clock::time_point postprocess_end = Clock::now();
        timing.preprocess_ms += Milliseconds(preprocess_start, preprocess_end);
        timing.inference_ms += Milliseconds(inference_start, inference_end);
        timing.postprocess_ms += Milliseconds(postprocess_start, postprocess_end);
    }

    std::vector<cv::dnn::Net> nets_;
    std::vector<std::string> labels_;
    int height_ = 48;
    int minimum_width_ = 320;
    int batch_size_ = 1;
    int concurrency_ = 1;
};

}  // namespace

struct OcrEngine::Impl {
    explicit Impl(const lw_ppocr_config& config)
        : log_level(config.log_level),
          log_callback(config.log_callback),
          log_user_data(config.log_user_data),
          enable_classifier(config.enable_classifier != 0),
          manifest(core::LoadModelManifest(config.model_manifest_utf8)),
          detector(manifest.detector, config),
          recognizer(manifest.recognizer, manifest.dictionary, config) {
        if (enable_classifier) {
            if (!manifest.has_classifier) {
                throw std::runtime_error(
                    "classifier is enabled but the model manifest has no classifier stage");
            }
            classifier = std::make_unique<Classifier>(manifest.classifier, config);
        }
    }

    lw_ppocr_log_level log_level = LW_PPOCR_LOG_OFF;
    lw_ppocr_log_callback log_callback = nullptr;
    void* log_user_data = nullptr;
    bool enable_classifier = false;
    core::ModelManifest manifest;
    Detector detector;
    std::unique_ptr<Classifier> classifier;
    Recognizer recognizer;
    std::mutex run_mutex;
};

OcrEngine::OcrEngine(const lw_ppocr_config& config)
    : impl_(std::make_unique<Impl>(config)) {
    Log(LW_PPOCR_LOG_INFO,
        "OpenCV DNN Runtime loaded model package: " + impl_->manifest.name);
}

OcrEngine::~OcrEngine() = default;

core::PipelineResult OcrEngine::Run(const cv::Mat& image) {
    std::lock_guard<std::mutex> lock(impl_->run_mutex);
    const Clock::time_point pipeline_start = Clock::now();
    if (image.empty() || image.type() != CV_8UC3) {
        throw std::invalid_argument("OCR input must be a non-empty BGR image");
    }

    core::PipelineResult result;
    result.regions = impl_->detector.Run(image, result.detector);

    std::vector<cv::Mat> crops;
    std::vector<core::OcrRegion> valid_regions;
    crops.reserve(result.regions.size());
    valid_regions.reserve(result.regions.size());
    for (core::OcrRegion& region : result.regions) {
        cv::Mat crop = core::GetRotateCropImage(image, region.box);
        if (!crop.empty()) {
            crops.push_back(std::move(crop));
            valid_regions.push_back(std::move(region));
        }
    }
    result.regions = std::move(valid_regions);

    if (impl_->classifier != nullptr) {
        impl_->classifier->Run(crops, result.regions, result.classifier);
    }
    impl_->recognizer.Run(crops, result.regions, result.recognizer);
    result.pipeline.total_ms = Milliseconds(pipeline_start, Clock::now());

    std::ostringstream message;
    message << "OpenCV DNN timing: det=" << result.detector.total_ms
            << " ms, cls=" << result.classifier.total_ms
            << " ms, rec=" << result.recognizer.total_ms
            << " ms, total=" << result.pipeline.total_ms << " ms";
    Log(LW_PPOCR_LOG_DEBUG, message.str());
    return result;
}

core::RecognitionResult OcrEngine::RecognizeBatch(
    const std::vector<cv::Mat>& images) {
    std::lock_guard<std::mutex> lock(impl_->run_mutex);
    const Clock::time_point pipeline_start = Clock::now();
    std::vector<cv::Mat> crops;
    crops.reserve(images.size());
    for (const cv::Mat& image : images) {
        if (image.empty() || image.type() != CV_8UC3) {
            throw std::invalid_argument(
                "recognition input must be a non-empty BGR image");
        }
        crops.push_back(image.clone());
    }

    core::RecognitionResult result;
    result.items.resize(crops.size());
    if (impl_->classifier != nullptr) {
        impl_->classifier->Run(crops, result.items, result.classifier);
    }
    impl_->recognizer.Run(crops, result.items, result.recognizer);
    result.pipeline.total_ms = Milliseconds(pipeline_start, Clock::now());
    return result;
}

void OcrEngine::Log(
    lw_ppocr_log_level level,
    const std::string& message) const noexcept {
    if (impl_ == nullptr || impl_->log_callback == nullptr ||
        level > impl_->log_level) {
        return;
    }
    try {
        impl_->log_callback(level, message.c_str(), impl_->log_user_data);
    } catch (...) {
    }
}

}  // namespace lw::ppocr::opencv_dnn
