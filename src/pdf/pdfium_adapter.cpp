#include "pdfium_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#else
#include <dlfcn.h>
#endif

namespace lw::ppocr::pdf {
namespace {

using FPDF_DOCUMENT = void*;
using FPDF_PAGE = void*;
using FPDF_BITMAP = void*;
using FPDF_TEXTPAGE = void*;
using FPDF_PAGEOBJECT = void*;
using FPDF_BOOL = int;

struct FpdfLibraryConfig {
    int version;
    const char** user_font_paths;
    void* isolate;
    unsigned int v8_embedder_slot;
    void* platform;
    int renderer_type;
    int font_library_type;
    FPDF_BOOL brotli_enabled;
};

struct FpdfRect {
    float left;
    float top;
    float right;
    float bottom;
};

enum {
    kBitmapBgr = 2,
    kPageObjectImage = 3,
    kTextRenderInvisible = 3,
};

using InitLibraryFn = void(*)(const FpdfLibraryConfig*);
using DestroyLibraryFn = void(*)();
using LoadMemDocument64Fn = FPDF_DOCUMENT(*)(const void*, size_t, const char*);
using LoadMemDocumentFn = FPDF_DOCUMENT(*)(const void*, int, const char*);
using GetPageCountFn = int(*)(FPDF_DOCUMENT);
using LoadPageFn = FPDF_PAGE(*)(FPDF_DOCUMENT, int);
using ClosePageFn = void(*)(FPDF_PAGE);
using CloseDocumentFn = void(*)(FPDF_DOCUMENT);
using GetPageWidthFn = double(*)(FPDF_PAGE);
using GetPageHeightFn = double(*)(FPDF_PAGE);
using GetPageRotationFn = int(*)(FPDF_PAGE);
using RenderPageBitmapFn = void(*)(FPDF_BITMAP, FPDF_PAGE, int, int,
    int, int, int, int);
using BitmapCreateExFn = FPDF_BITMAP(*)(int, int, int, void*, int);
using BitmapDestroyFn = void(*)(FPDF_BITMAP);
using BitmapFillRectFn = FPDF_BOOL(*)(FPDF_BITMAP, int, int, int, int,
    uint32_t);
using PageToDeviceFn = FPDF_BOOL(*)(FPDF_PAGE, int, int, int, int, int,
    double, double, int*, int*);
using TextLoadPageFn = FPDF_TEXTPAGE(*)(FPDF_PAGE);
using TextClosePageFn = void(*)(FPDF_TEXTPAGE);
using TextCountCharsFn = int(*)(FPDF_TEXTPAGE);
using TextGetUnicodeFn = unsigned int(*)(FPDF_TEXTPAGE, int);
using TextHasUnicodeMapErrorFn = int(*)(FPDF_TEXTPAGE, int);
using TextGetCharBoxFn = FPDF_BOOL(*)(FPDF_TEXTPAGE, int, double*, double*,
    double*, double*);
using TextGetTextObjectFn = FPDF_PAGEOBJECT(*)(FPDF_TEXTPAGE, int);
using TextObjGetRenderModeFn = int(*)(FPDF_PAGEOBJECT);
using GetPageBoundingBoxFn = FPDF_BOOL(*)(FPDF_PAGE, FpdfRect*);
using PageCountObjectsFn = int(*)(FPDF_PAGE);
using PageGetObjectFn = FPDF_PAGEOBJECT(*)(FPDF_PAGE, int);
using PageObjGetTypeFn = int(*)(FPDF_PAGEOBJECT);
using PageObjGetBoundsFn = FPDF_BOOL(*)(FPDF_PAGEOBJECT, float*, float*,
    float*, float*);

#if defined(_WIN32)
using LibraryHandle = HMODULE;
#else
using LibraryHandle = void*;
#endif

template <typename Function>
Function LoadSymbol(LibraryHandle library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<Function>(GetProcAddress(library, name));
#else
    return reinterpret_cast<Function>(dlsym(library, name));
#endif
}

LibraryHandle OpenLibrary(const std::string& path) {
#if defined(_WIN32)
    if (path.size() >= static_cast<size_t>(
            (std::numeric_limits<int>::max)())) {
        return nullptr;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    if (required <= 0) return nullptr;
    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
            wide.data(), required) <= 0) {
        return nullptr;
    }
    return LoadLibraryW(wide.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

#if defined(_WIN32)
LibraryHandle OpenLibrary(const std::wstring& path) {
    return LoadLibraryW(path.c_str());
}
#endif

void CloseLibrary(LibraryHandle library) {
    if (library == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(library);
#else
    dlclose(library);
#endif
}

std::string DefaultLibraryName() {
#if defined(_WIN32)
    return "pdfium.dll";
#elif defined(__APPLE__)
    return "libpdfium.dylib";
#else
    return "libpdfium.so";
#endif
}

struct Api final {
    LibraryHandle library = nullptr;
    bool load_attempted = false;
    std::string load_error;
    InitLibraryFn init_library = nullptr;
    DestroyLibraryFn destroy_library = nullptr;
    LoadMemDocument64Fn load_mem_document64 = nullptr;
    LoadMemDocumentFn load_mem_document = nullptr;
    GetPageCountFn get_page_count = nullptr;
    LoadPageFn load_page = nullptr;
    ClosePageFn close_page = nullptr;
    CloseDocumentFn close_document = nullptr;
    GetPageWidthFn get_page_width = nullptr;
    GetPageHeightFn get_page_height = nullptr;
    GetPageRotationFn get_page_rotation = nullptr;
    RenderPageBitmapFn render_page_bitmap = nullptr;
    BitmapCreateExFn bitmap_create_ex = nullptr;
    BitmapDestroyFn bitmap_destroy = nullptr;
    BitmapFillRectFn bitmap_fill_rect = nullptr;
    PageToDeviceFn page_to_device = nullptr;
    TextLoadPageFn text_load_page = nullptr;
    TextClosePageFn text_close_page = nullptr;
    TextCountCharsFn text_count_chars = nullptr;
    TextGetUnicodeFn text_get_unicode = nullptr;
    TextHasUnicodeMapErrorFn text_has_unicode_map_error = nullptr;
    TextGetCharBoxFn text_get_char_box = nullptr;
    TextGetTextObjectFn text_get_text_object = nullptr;
    TextObjGetRenderModeFn text_obj_get_render_mode = nullptr;
    GetPageBoundingBoxFn get_page_bounding_box = nullptr;
    PageCountObjectsFn page_count_objects = nullptr;
    PageGetObjectFn page_get_object = nullptr;
    PageObjGetTypeFn page_obj_get_type = nullptr;
    PageObjGetBoundsFn page_obj_get_bounds = nullptr;
    bool initialized = false;

    ~Api() {
        if (initialized && destroy_library != nullptr) destroy_library();
        CloseLibrary(library);
    }

    bool Load(std::string& error) {
        if (library != nullptr) return true;
        if (load_attempted) {
            error = load_error;
            return false;
        }
        load_attempted = true;
        std::string path;
#if defined(_WIN32)
        std::wstring wide_path;
        wchar_t* wide_value = nullptr;
        size_t wide_value_length = 0;
        _wdupenv_s(&wide_value, &wide_value_length,
            L"LW_PPOCR_PDFIUM_LIBRARY");
        if (wide_value != nullptr && *wide_value != L'\0') {
            wide_path.assign(wide_value, wide_value_length);
            path = "<LW_PPOCR_PDFIUM_LIBRARY>";
        } else {
            path = DefaultLibraryName();
        }
        std::free(wide_value);
#else
        if (const char* value = std::getenv("LW_PPOCR_PDFIUM_LIBRARY");
            value != nullptr && *value != '\0') {
            path = value;
        } else {
            path = DefaultLibraryName();
        }
#endif
#if defined(_WIN32)
        library = wide_path.empty() ? OpenLibrary(path) : OpenLibrary(wide_path);
#else
        library = OpenLibrary(path);
#endif
        if (library == nullptr) {
            error = "unable to load PDFium library: " + path;
            load_error = error;
            return false;
        }

#define LW_PDFIUM_REQUIRED(field, exported_name, type) \
        field = LoadSymbol<type>(library, exported_name); \
        if (field == nullptr) { \
            error = "PDFium library is missing symbol: " exported_name; \
            load_error = error; \
            CloseLibrary(library); library = nullptr; return false; \
        }

#define LW_PDFIUM_REQUIRED_LIST \
        LW_PDFIUM_REQUIRED(init_library, "FPDF_InitLibraryWithConfig", InitLibraryFn) \
        LW_PDFIUM_REQUIRED(destroy_library, "FPDF_DestroyLibrary", DestroyLibraryFn) \
        LW_PDFIUM_REQUIRED(get_page_count, "FPDF_GetPageCount", GetPageCountFn) \
        LW_PDFIUM_REQUIRED(load_page, "FPDF_LoadPage", LoadPageFn) \
        LW_PDFIUM_REQUIRED(close_page, "FPDF_ClosePage", ClosePageFn) \
        LW_PDFIUM_REQUIRED(close_document, "FPDF_CloseDocument", CloseDocumentFn) \
        LW_PDFIUM_REQUIRED(get_page_width, "FPDF_GetPageWidth", GetPageWidthFn) \
        LW_PDFIUM_REQUIRED(get_page_height, "FPDF_GetPageHeight", GetPageHeightFn) \
        LW_PDFIUM_REQUIRED(render_page_bitmap, "FPDF_RenderPageBitmap", RenderPageBitmapFn) \
        LW_PDFIUM_REQUIRED(bitmap_create_ex, "FPDFBitmap_CreateEx", BitmapCreateExFn) \
        LW_PDFIUM_REQUIRED(bitmap_destroy, "FPDFBitmap_Destroy", BitmapDestroyFn) \
        LW_PDFIUM_REQUIRED(bitmap_fill_rect, "FPDFBitmap_FillRect", BitmapFillRectFn) \
        LW_PDFIUM_REQUIRED(page_to_device, "FPDF_PageToDevice", PageToDeviceFn) \
        LW_PDFIUM_REQUIRED(text_load_page, "FPDFText_LoadPage", TextLoadPageFn) \
        LW_PDFIUM_REQUIRED(text_close_page, "FPDFText_ClosePage", TextClosePageFn) \
        LW_PDFIUM_REQUIRED(text_count_chars, "FPDFText_CountChars", TextCountCharsFn) \
        LW_PDFIUM_REQUIRED(text_get_unicode, "FPDFText_GetUnicode", TextGetUnicodeFn) \
        LW_PDFIUM_REQUIRED(text_get_char_box, "FPDFText_GetCharBox", TextGetCharBoxFn) \
        LW_PDFIUM_REQUIRED(load_mem_document64, "FPDF_LoadMemDocument64", LoadMemDocument64Fn)

        LW_PDFIUM_REQUIRED_LIST
#undef LW_PDFIUM_REQUIRED_LIST
#undef LW_PDFIUM_REQUIRED

        load_mem_document = LoadSymbol<LoadMemDocumentFn>(
            library, "FPDF_LoadMemDocument");
        text_has_unicode_map_error = LoadSymbol<TextHasUnicodeMapErrorFn>(
            library, "FPDFText_HasUnicodeMapError");
        text_get_text_object = LoadSymbol<TextGetTextObjectFn>(
            library, "FPDFText_GetTextObject");
        get_page_rotation = LoadSymbol<GetPageRotationFn>(
            library, "FPDFPage_GetRotation");
        text_obj_get_render_mode = LoadSymbol<TextObjGetRenderModeFn>(
            library, "FPDFTextObj_GetTextRenderMode");
        get_page_bounding_box = LoadSymbol<GetPageBoundingBoxFn>(
            library, "FPDF_GetPageBoundingBox");
        page_count_objects = LoadSymbol<PageCountObjectsFn>(
            library, "FPDFPage_CountObjects");
        page_get_object = LoadSymbol<PageGetObjectFn>(
            library, "FPDFPage_GetObject");
        page_obj_get_type = LoadSymbol<PageObjGetTypeFn>(
            library, "FPDFPageObj_GetType");
        page_obj_get_bounds = LoadSymbol<PageObjGetBoundsFn>(
            library, "FPDFPageObj_GetBounds");

        FpdfLibraryConfig config{};
        config.version = 2;
        init_library(&config);
        initialized = true;
        load_error.clear();
        return true;
    }
};

Api& GetApi() {
    static Api api;
    return api;
}

std::mutex& ApiMutex() {
    static std::mutex mutex;
    return mutex;
}

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int NormalizeRotation(int rotation) {
    rotation %= 4;
    return rotation < 0 ? rotation + 4 : rotation;
}

// The caller must hold ApiMutex(); use RequireApi() for an unlocked caller.
void RequireApiLocked() {
    std::string error;
    if (!GetApi().Load(error)) throw std::runtime_error(error);
}

void RequireApi() {
    std::lock_guard<std::mutex> lock(ApiMutex());
    RequireApiLocked();
}

void AppendUtf8(std::string& output, uint32_t codepoint) {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return;
    if (codepoint > 0x10FFFF) codepoint = 0xFFFD;
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

std::string CodepointToUtf8(uint32_t codepoint) {
    std::string result;
    AppendUtf8(result, codepoint);
    return result;
}

struct Character {
    std::string text;
    std::vector<cv::Point2f> box;
    float center_y = 0.0f;
    float center_x = 0.0f;
    float height = 0.0f;
    bool visible = true;
};

std::vector<cv::Point2f> MapBox(
    Api& api,
    FPDF_PAGE page,
    double left,
    double right,
    double bottom,
    double top,
    int width,
    int height,
    int rotation) {
    const std::pair<double, double> page_points[] = {
        {left, bottom}, {right, bottom}, {right, top}, {left, top}
    };
    std::vector<cv::Point2f> points;
    points.reserve(4);
    for (const auto& point : page_points) {
        int x = 0;
        int y = 0;
        if (!api.page_to_device(page, 0, 0, width, height, rotation,
                point.first, point.second, &x, &y)) {
            return {};
        }
        points.emplace_back(static_cast<float>(x), static_cast<float>(y));
    }
    return points;
}

std::vector<TextItem> GroupCharacters(std::vector<Character> characters) {
    if (characters.empty()) return {};
    std::stable_sort(characters.begin(), characters.end(),
        [](const Character& left, const Character& right) {
            if (std::abs(left.center_y - right.center_y) >
                std::max(left.height, right.height) * 0.4f) {
                return left.center_y < right.center_y;
            }
            return left.center_x < right.center_x;
        });

    std::vector<TextItem> lines;
    for (const Character& character : characters) {
        const bool whitespace = character.text == " " ||
            character.text == "\t" || character.text == "\n";
        if (whitespace && lines.empty()) continue;
        if (lines.empty()) {
            lines.push_back({character.text, character.box, character.visible});
            continue;
        }
        TextItem& current = lines.back();
        float min_y = std::numeric_limits<float>::max();
        float max_y = std::numeric_limits<float>::lowest();
        float max_x = std::numeric_limits<float>::lowest();
        for (const auto& point : current.box) {
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
            max_x = std::max(max_x, point.x);
        }
        const bool same_line = std::abs(character.center_y -
            (min_y + max_y) * 0.5f) <= std::max(character.height, 3.0f) * 0.75f;
        if (!same_line || (!whitespace && character.center_x < max_x - 2.0f)) {
            lines.push_back({character.text, character.box, character.visible});
            continue;
        }
        if (!whitespace && !current.text.empty() &&
            character.center_x - max_x > std::max(character.height, 3.0f) * 1.5f) {
            current.text.push_back(' ');
        }
        if (!whitespace) current.text += character.text;
        current.visible = current.visible && character.visible;
        current.box.insert(current.box.end(), character.box.begin(), character.box.end());
    }

    for (TextItem& line : lines) {
        if (line.box.empty()) continue;
        float left = std::numeric_limits<float>::max();
        float top = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float bottom = std::numeric_limits<float>::lowest();
        for (const auto& point : line.box) {
            left = std::min(left, point.x);
            top = std::min(top, point.y);
            right = std::max(right, point.x);
            bottom = std::max(bottom, point.y);
        }
        line.box = {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
    }
    lines.erase(std::remove_if(lines.begin(), lines.end(),
        [](const TextItem& item) {
            return item.text.empty() || std::all_of(item.text.begin(),
                item.text.end(), [](unsigned char value) {
                    return std::isspace(value) != 0;
                });
        }), lines.end());
    return lines;
}

}  // namespace

class Document::Impl final {
public:
    Impl(const uint8_t* data, uint64_t size, const Options& options)
        : data_(data), data_size_(size), options_(options) {
        if (data == nullptr || size < 5 || size >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
            throw std::invalid_argument("PDF data is empty or too large");
        }
        if (std::memcmp(data_, "%PDF-", 5) != 0) {
            throw std::invalid_argument("PDF signature is missing");
        }
        if (options_.dpi < 36 || options_.dpi > 600 || options_.max_pages == 0 ||
            options_.max_page_pixels == 0 || options_.max_total_pixels == 0) {
            throw std::invalid_argument("PDF options are invalid");
        }

        std::lock_guard<std::mutex> lock(ApiMutex());
        RequireApiLocked();
        Api& api = GetApi();
        document_ = api.load_mem_document64(data_, data_size_, nullptr);
        if (document_ == nullptr) {
            throw std::runtime_error("PDFium could not open the PDF document");
        }
        page_count_ = api.get_page_count(document_);
        if (page_count_ < 1) {
            api.close_document(document_);
            document_ = nullptr;
            throw std::runtime_error("PDF document has no pages");
        }
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(ApiMutex());
        if (document_ != nullptr && GetApi().close_document != nullptr) {
            GetApi().close_document(document_);
            document_ = nullptr;
        }
    }

    uint32_t page_count() const noexcept {
        return static_cast<uint32_t>(page_count_);
    }

    Page process_page(uint32_t page_index, bool extract_text, bool render) {
        std::lock_guard<std::mutex> lock(ApiMutex());
        Api& api = GetApi();
        if (page_index >= static_cast<uint32_t>(page_count_)) {
            throw std::out_of_range("PDF page index is out of range");
        }
        FPDF_PAGE page = api.load_page(document_, static_cast<int>(page_index));
        if (page == nullptr) throw std::runtime_error("PDF page could not be loaded");
        try {
            Page result;
            result.page_index = page_index;
            const double page_width = api.get_page_width(page);
            const double page_height = api.get_page_height(page);
            const int rotation = api.get_page_rotation == nullptr
                ? 0 : NormalizeRotation(api.get_page_rotation(page));
            const double scale = static_cast<double>(options_.dpi) / 72.0;
            int width = static_cast<int>(std::ceil(page_width * scale));
            int height = static_cast<int>(std::ceil(page_height * scale));
            if (rotation % 2 != 0) std::swap(width, height);
            if (width < 1 || height < 1) {
                throw std::runtime_error("PDF page dimensions are invalid");
            }
            const uint64_t pixels = static_cast<uint64_t>(width) * height;
            if (pixels > options_.max_page_pixels) {
                throw std::runtime_error("PDF page exceeds max_page_pixels");
            }
            result.width = width;
            result.height = height;
            result.has_large_image = HasLargeImage(api, page, page_width, page_height);

            if (extract_text) {
                const auto start = Clock::now();
                result.text = ExtractText(api, page, width, height, rotation,
                    result.text_layer_usable);
                result.text_extract_ms = ElapsedMs(start);
            }
            if (render) {
                const auto start = Clock::now();
                result.image = Render(api, page, width, height, rotation);
                result.render_ms = ElapsedMs(start);
            }
            api.close_page(page);
            return result;
        } catch (...) {
            api.close_page(page);
            throw;
        }
    }

private:
    static bool HasLargeImage(Api& api, FPDF_PAGE page, double page_width,
                              double page_height) {
        if (api.page_count_objects == nullptr || api.page_get_object == nullptr ||
            api.page_obj_get_type == nullptr || api.page_obj_get_bounds == nullptr ||
            page_width <= 0 || page_height <= 0) {
            return false;
        }
        const int count = api.page_count_objects(page);
        if (count <= 0) return false;
        double area = 0.0;
        for (int index = 0; index < count; ++index) {
            FPDF_PAGEOBJECT object = api.page_get_object(page, index);
            if (object == nullptr || api.page_obj_get_type(object) != kPageObjectImage) {
                continue;
            }
            float left = 0.0f;
            float bottom = 0.0f;
            float right = 0.0f;
            float top = 0.0f;
            if (api.page_obj_get_bounds(object, &left, &bottom, &right, &top)) {
                area += std::max(0.0f, right - left) *
                    std::max(0.0f, top - bottom);
            }
        }
        return area / (page_width * page_height) >= 0.30;
    }

    static cv::Mat Render(Api& api, FPDF_PAGE page, int width, int height,
                          int rotation) {
        cv::Mat image(height, width, CV_8UC3, cv::Scalar(255, 255, 255));
        FPDF_BITMAP bitmap = api.bitmap_create_ex(
            width, height, kBitmapBgr, image.data, static_cast<int>(image.step));
        if (bitmap == nullptr) throw std::runtime_error("PDF bitmap allocation failed");
        api.bitmap_fill_rect(bitmap, 0, 0, width, height, 0xFFFFFFFFu);
        api.render_page_bitmap(bitmap, page, 0, 0, width, height,
            rotation, 0);
        api.bitmap_destroy(bitmap);
        return image;
    }

    static std::vector<TextItem> ExtractText(Api& api, FPDF_PAGE page, int width,
                                             int height, int rotation,
                                             bool& usable) {
        usable = false;
        FPDF_TEXTPAGE text_page = api.text_load_page(page);
        if (text_page == nullptr) return {};
        const int count = api.text_count_chars(text_page);
        std::vector<Character> characters;
        int valid = 0;
        int visible = 0;
        if (count > 0) characters.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            const unsigned int unicode = api.text_get_unicode(text_page, index);
            const int map_error = api.text_has_unicode_map_error == nullptr
                ? 0 : api.text_has_unicode_map_error(text_page, index);
            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (unicode == 0 || map_error != 0 ||
                !api.text_get_char_box(text_page, index, &left, &right,
                    &bottom, &top) || right <= left || top <= bottom) {
                continue;
            }
            std::vector<cv::Point2f> box = MapBox(api, page, left, right,
                bottom, top, width, height, rotation);
            if (box.size() != 4) continue;
            const FPDF_PAGEOBJECT text_object = api.text_get_text_object == nullptr
                ? nullptr : api.text_get_text_object(text_page, index);
            bool is_visible = true;
            if (text_object != nullptr && api.text_obj_get_render_mode != nullptr) {
                is_visible = api.text_obj_get_render_mode(text_object) !=
                    kTextRenderInvisible;
            }
            float min_x = std::numeric_limits<float>::max();
            float min_y = std::numeric_limits<float>::max();
            float max_x = std::numeric_limits<float>::lowest();
            float max_y = std::numeric_limits<float>::lowest();
            for (const auto& point : box) {
                min_x = std::min(min_x, point.x);
                min_y = std::min(min_y, point.y);
                max_x = std::max(max_x, point.x);
                max_y = std::max(max_y, point.y);
            }
            const std::string text = CodepointToUtf8(unicode);
            if (text.empty()) continue;
            ++valid;
            if (is_visible) ++visible;
            characters.push_back({text, std::move(box),
                (min_y + max_y) * 0.5f, (min_x + max_x) * 0.5f,
                max_y - min_y, is_visible});
        }
        api.text_close_page(text_page);
        usable = valid >= 3 && visible >= 1 &&
            (count <= 0 || static_cast<double>(valid) / count >= 0.80);
        return GroupCharacters(std::move(characters));
    }

    const uint8_t* data_ = nullptr;
    uint64_t data_size_ = 0;
    Options options_;
    FPDF_DOCUMENT document_ = nullptr;
    int page_count_ = 0;
};

Document::Document(const uint8_t* data, uint64_t size, const Options& options)
    : impl_(std::make_unique<Impl>(data, size, options)) {}

Document::~Document() = default;

uint32_t Document::page_count() const noexcept {
    return impl_->page_count();
}

Page Document::process_page(uint32_t page_index, bool extract_text, bool render) {
    return impl_->process_page(page_index, extract_text, render);
}

bool IsAvailable() {
    try {
        std::lock_guard<std::mutex> lock(ApiMutex());
        std::string error;
        return GetApi().Load(error);
    } catch (...) {
        return false;
    }
}

}  // namespace lw::ppocr::pdf
