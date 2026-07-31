#include <lw/ppocr.h>

#include <cstring>

int main() {
    lw_ppocr_config config{};
    lw_ppocr_config_init(&config);
    if (config.struct_size != sizeof(config) ||
        config.api_version != LW_PPOCR_API_VERSION) {
        return 1;
    }
    lw_ppocr_version version{};
    version.struct_size = sizeof(version);
    if (lw_ppocr_get_version(&version) != LW_PPOCR_STATUS_OK ||
        version.api_version != LW_PPOCR_API_VERSION ||
        std::strcmp(version.version_utf8, LW_PPOCR_VERSION_STRING) != 0) {
        return 2;
    }
    return 0;
}
