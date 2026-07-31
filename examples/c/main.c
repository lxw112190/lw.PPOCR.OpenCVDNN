#include <lw/ppocr.h>

#include <stdio.h>
#include <stdlib.h>

static unsigned char* read_file(const char* path, size_t* size) {
    FILE* input = fopen(path, "rb");
    unsigned char* data;
    long length;
    if (input == NULL) return NULL;
    if (fseek(input, 0, SEEK_END) != 0 || (length = ftell(input)) <= 0 ||
        fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        return NULL;
    }
    data = (unsigned char*)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, input) != (size_t)length) {
        free(data);
        fclose(input);
        return NULL;
    }
    fclose(input);
    *size = (size_t)length;
    return data;
}

static void print_last_error(lw_ppocr_handle handle) {
    char message[2048];
    lw_ppocr_get_last_error(handle, message, sizeof(message));
    fprintf(stderr, "%s\n", message);
}

int main(int argc, char** argv) {
    lw_ppocr_config config;
    lw_ppocr_handle handle = NULL;
    unsigned char* image;
    size_t image_size = 0;
    char* result = NULL;
    uint64_t result_length = 0;
    lw_ppocr_status status;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <model.json> <image>\n", argv[0]);
        return 2;
    }
    image = read_file(argv[2], &image_size);
    if (image == NULL) {
        fprintf(stderr, "Unable to read image: %s\n", argv[2]);
        return 2;
    }

    lw_ppocr_config_init(&config);
    config.model_manifest_utf8 = argv[1];
    status = lw_ppocr_create(&config, &handle);
    if (status != LW_PPOCR_STATUS_OK) {
        print_last_error(handle);
        free(image);
        return 1;
    }
    status = lw_ppocr_ocr_encoded(handle, image, (uint64_t)image_size,
        &result, &result_length);
    free(image);
    if (status != LW_PPOCR_STATUS_OK) {
        print_last_error(handle);
        lw_ppocr_destroy(&handle);
        return 1;
    }
    fwrite(result, 1, (size_t)result_length, stdout);
    fputc('\n', stdout);
    lw_ppocr_string_free(result);
    lw_ppocr_destroy(&handle);
    return 0;
}
