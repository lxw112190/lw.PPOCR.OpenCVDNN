#pragma once

#include <lw/ppocr.h>

#include <cstddef>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace lw::ppocr::http {

struct LoggingConfig {
    bool enabled = true;
    std::string level = "info";
    bool console = true;
    bool file_enabled = true;
    std::string file_path = "logs/lw-ppocr.log";
    bool request_enabled = true;
    size_t max_file_size = 10u * 1024u * 1024u;
    size_t max_files = 5;
};

void ConfigureLogging(const LoggingConfig& config);
std::shared_ptr<spdlog::logger> Logger();
bool RequestLoggingEnabled();
void ShutdownLogging();

void LW_PPOCR_CALL CoreLogBridge(
    lw_ppocr_log_level level,
    const char* message_utf8,
    void* user_data);

}  // namespace lw::ppocr::http
