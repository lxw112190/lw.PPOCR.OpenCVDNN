#include "logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <vector>

namespace lw::ppocr::http {
namespace {

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;
bool g_request_logging = false;

spdlog::level::level_enum ParseLevel(const std::string& value) {
    if (value == "trace") return spdlog::level::trace;
    if (value == "debug") return spdlog::level::debug;
    if (value == "warn" || value == "warning") return spdlog::level::warn;
    if (value == "error") return spdlog::level::err;
    if (value == "critical") return spdlog::level::critical;
    if (value == "off") return spdlog::level::off;
    return spdlog::level::info;
}

}  // namespace

void ConfigureLogging(const LoggingConfig& config) {
    std::lock_guard<std::mutex> lock(g_mutex);
    spdlog::drop("lw-ppocr");

    if (!config.enabled) {
        auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        g_logger = std::make_shared<spdlog::logger>("lw-ppocr", sink);
        g_logger->set_level(spdlog::level::off);
        g_request_logging = false;
        spdlog::register_logger(g_logger);
        return;
    }

    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (config.file_enabled) {
        const std::filesystem::path path =
            std::filesystem::u8path(config.file_path);
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file_path, config.max_file_size, config.max_files));
    }
    if (sinks.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
    }

    g_logger = std::make_shared<spdlog::logger>(
        "lw-ppocr", sinks.begin(), sinks.end());
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    g_logger->set_level(ParseLevel(config.level));
    g_request_logging = config.request_enabled;
    // Request-start records are diagnostic breadcrumbs for abrupt failures.
    // Flush INFO synchronously when they are enabled so the last request is
    // not left only in a userspace buffer.
    g_logger->flush_on(config.request_enabled
        ? spdlog::level::info
        : spdlog::level::warn);
    spdlog::register_logger(g_logger);
}

std::shared_ptr<spdlog::logger> Logger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_logger;
}

bool RequestLoggingEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_request_logging;
}

void ShutdownLogging() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logger != nullptr) {
        g_logger->flush();
    }
    spdlog::drop("lw-ppocr");
    g_logger.reset();
    g_request_logging = false;
}

void LW_PPOCR_CALL CoreLogBridge(
    lw_ppocr_log_level level,
    const char* message_utf8,
    void*) {
    const auto logger = Logger();
    if (logger == nullptr || message_utf8 == nullptr) {
        return;
    }
    switch (level) {
    case LW_PPOCR_LOG_ERROR:
        logger->error("{}", message_utf8);
        break;
    case LW_PPOCR_LOG_WARNING:
        logger->warn("{}", message_utf8);
        break;
    case LW_PPOCR_LOG_DEBUG:
        logger->debug("{}", message_utf8);
        break;
    default:
        logger->info("{}", message_utf8);
        break;
    }
}

}  // namespace lw::ppocr::http
