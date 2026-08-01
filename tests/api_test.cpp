#include <lw/ppocr.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert(std::is_standard_layout<lw_ppocr_version>::value,
    "lw_ppocr_version must remain a C ABI structure");
static_assert(std::is_standard_layout<lw_ppocr_config>::value,
    "lw_ppocr_config must remain a C ABI structure");
static_assert(offsetof(lw_ppocr_version, struct_size) == 0, "ABI changed");
static_assert(offsetof(lw_ppocr_version, api_version) == 4, "ABI changed");
static_assert(offsetof(lw_ppocr_config, struct_size) == 0, "ABI changed");
static_assert(offsetof(lw_ppocr_config, api_version) == 4, "ABI changed");
static_assert(offsetof(lw_ppocr_config, model_manifest_utf8) == 8, "ABI changed");

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(lw_ppocr_version) == 40, "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_version, product_name_utf8) == 24,
    "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_version, version_utf8) == 32,
    "64-bit ABI changed");
static_assert(sizeof(lw_ppocr_config) == 152, "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_config, max_image_pixels) == 56,
    "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_config, log_callback) == 72,
    "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_config, reserved_i32) == 88,
    "64-bit ABI changed");
static_assert(offsetof(lw_ppocr_config, reserved_ptr) == 120,
    "64-bit ABI changed");
#endif

using create_signature = lw_ppocr_status(LW_PPOCR_CALL*)(
    const lw_ppocr_config*, lw_ppocr_handle*);
using ocr_signature = lw_ppocr_status(LW_PPOCR_CALL*)(
    lw_ppocr_handle, const uint8_t*, uint64_t, char**, uint64_t*);
static_assert(std::is_same<decltype(&lw_ppocr_create), create_signature>::value,
    "lw_ppocr_create signature changed");
static_assert(std::is_same<decltype(&lw_ppocr_ocr_encoded), ocr_signature>::value,
    "lw_ppocr_ocr_encoded signature changed");

int main() {
    if (LW_PPOCR_API_VERSION != 1u ||
        LW_PPOCR_STATUS_OK != 0 ||
        LW_PPOCR_STATUS_INVALID_ARGUMENT != -1 ||
        LW_PPOCR_STATUS_INTERNAL_ERROR != -6 ||
        LW_PPOCR_LOG_OFF != 0 || LW_PPOCR_LOG_DEBUG != 4) {
        return 1;
    }
    lw_ppocr_config config{};
    lw_ppocr_config_init(&config);
    if (config.struct_size != sizeof(config) ||
        config.api_version != LW_PPOCR_API_VERSION) {
        return 2;
    }
    for (int32_t value : config.reserved_i32) {
        if (value != 0) return 3;
    }
    for (const void* value : config.reserved_ptr) {
        if (value != nullptr) return 4;
    }
    lw_ppocr_version version{};
    version.struct_size = sizeof(version);
    if (lw_ppocr_get_version(&version) != LW_PPOCR_STATUS_OK ||
        version.api_version != LW_PPOCR_API_VERSION ||
        std::strcmp(version.version_utf8, LW_PPOCR_VERSION_STRING) != 0) {
        return 5;
    }
    return 0;
}
