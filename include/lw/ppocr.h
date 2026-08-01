#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define LW_PPOCR_CALL __cdecl
#if defined(LW_PPOCR_BUILDING_LIBRARY)
#define LW_PPOCR_API __declspec(dllexport)
#else
#define LW_PPOCR_API __declspec(dllimport)
#endif
#else
#define LW_PPOCR_CALL
#if defined(LW_PPOCR_BUILDING_LIBRARY) && defined(__GNUC__)
#define LW_PPOCR_API __attribute__((visibility("default")))
#else
#define LW_PPOCR_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LW_PPOCR_API_VERSION 1u
#define LW_PPOCR_VERSION_MAJOR 1u
#define LW_PPOCR_VERSION_MINOR 0u
#define LW_PPOCR_VERSION_PATCH 0u
#define LW_PPOCR_VERSION_STRING "1.0.0-rc.2"
#define LW_PPOCR_PRODUCT_NAME "lw.PPOCR.OpenCVDNN"

typedef struct lw_ppocr_engine* lw_ppocr_handle;
typedef int32_t lw_ppocr_status;
typedef int32_t lw_ppocr_log_level;

enum {
    LW_PPOCR_STATUS_OK = 0,
    LW_PPOCR_STATUS_INVALID_ARGUMENT = -1,
    LW_PPOCR_STATUS_MODEL_ERROR = -2,
    LW_PPOCR_STATUS_IMAGE_ERROR = -3,
    LW_PPOCR_STATUS_INFERENCE_ERROR = -4,
    LW_PPOCR_STATUS_OUT_OF_MEMORY = -5,
    LW_PPOCR_STATUS_INTERNAL_ERROR = -6
};

enum {
    LW_PPOCR_LOG_OFF = 0,
    LW_PPOCR_LOG_ERROR = 1,
    LW_PPOCR_LOG_WARNING = 2,
    LW_PPOCR_LOG_INFO = 3,
    LW_PPOCR_LOG_DEBUG = 4
};

typedef void(LW_PPOCR_CALL* lw_ppocr_log_callback)(
    lw_ppocr_log_level level,
    const char* message_utf8,
    void* user_data);

typedef struct lw_ppocr_version {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    const char* product_name_utf8;
    const char* version_utf8;
} lw_ppocr_version;

typedef struct lw_ppocr_config {
    uint32_t struct_size;
    uint32_t api_version;
    const char* model_manifest_utf8;

    int32_t enable_classifier;
    int32_t limit_side_len;
    float det_db_threshold;
    float det_db_box_threshold;
    float det_db_unclip_ratio;
    int32_t det_use_dilation;
    float cls_threshold;
    int32_t cls_batch_size;
    int32_t rec_batch_size;
    int32_t rec_concurrency;
    uint64_t max_image_pixels;

    lw_ppocr_log_level log_level;
    lw_ppocr_log_callback log_callback;
    void* log_user_data;

    int32_t reserved_i32[8];
    const void* reserved_ptr[4];
} lw_ppocr_config;

/* Fills a configuration with safe defaults. Set model_manifest_utf8 before create. */
LW_PPOCR_API void LW_PPOCR_CALL lw_ppocr_config_init(lw_ppocr_config* config);

LW_PPOCR_API lw_ppocr_status LW_PPOCR_CALL lw_ppocr_get_version(
    lw_ppocr_version* version);

LW_PPOCR_API lw_ppocr_status LW_PPOCR_CALL lw_ppocr_create(
    const lw_ppocr_config* config,
    lw_ppocr_handle* handle);

/* Runs detection, optional direction classification, and recognition. */
LW_PPOCR_API lw_ppocr_status LW_PPOCR_CALL lw_ppocr_ocr_encoded(
    lw_ppocr_handle handle,
    const uint8_t* encoded_image,
    uint64_t encoded_size,
    char** result_json_utf8,
    uint64_t* result_json_length);

/* Recognizes one already-cropped text-line image without detection. */
LW_PPOCR_API lw_ppocr_status LW_PPOCR_CALL lw_ppocr_recognize_encoded(
    lw_ppocr_handle handle,
    const uint8_t* encoded_image,
    uint64_t encoded_size,
    char** result_json_utf8,
    uint64_t* result_json_length);

/* Recognizes 1..256 already-cropped text-line images as one ordered batch. */
LW_PPOCR_API lw_ppocr_status LW_PPOCR_CALL lw_ppocr_recognize_batch_encoded(
    lw_ppocr_handle handle,
    const uint8_t* const* encoded_images,
    const uint64_t* encoded_sizes,
    uint64_t image_count,
    char** result_json_utf8,
    uint64_t* result_json_length);

LW_PPOCR_API void LW_PPOCR_CALL lw_ppocr_string_free(char* value);

/* Returns the required UTF-8 buffer size including the trailing NUL. */
LW_PPOCR_API uint64_t LW_PPOCR_CALL lw_ppocr_get_last_error(
    lw_ppocr_handle handle,
    char* buffer_utf8,
    uint64_t buffer_capacity);

LW_PPOCR_API void LW_PPOCR_CALL lw_ppocr_destroy(lw_ppocr_handle* handle);

#ifdef __cplusplus
}
#endif
