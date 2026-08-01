#pragma once

#include <lw/ppocr.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace spdlog {
class logger;
}

namespace lw::ppocr::http {

enum class AccessLogFormat {
    Text,
    JsonLines
};

struct LoggingConfig {
    bool enabled = true;
    std::string level = "info";
    bool console = true;
    bool file_enabled = true;
    std::string file_path = "logs/runtime.log";
    bool request_enabled = true;
    bool request_start_enabled = true;
    bool access_file_enabled = true;
    std::string access_file_path = "logs/access.log";
    std::string access_format = "text";
    size_t flush_interval_seconds = 1;
    std::vector<std::string> trusted_proxies;
    size_t max_file_size = 10u * 1024u * 1024u;
    size_t max_files = 5;
};

void ConfigureLogging(const LoggingConfig& config);
std::shared_ptr<spdlog::logger> Logger();
std::shared_ptr<spdlog::logger> AccessLogger();
bool RequestLoggingEnabled();
bool RequestStartLoggingEnabled();
AccessLogFormat ConfiguredAccessLogFormat();
void FlushRuntimeLogging();
void ShutdownLogging();

void LW_PPOCR_CALL CoreLogBridge(
    lw_ppocr_log_level level,
    const char* message_utf8,
    void* user_data);

}  // namespace lw::ppocr::http
