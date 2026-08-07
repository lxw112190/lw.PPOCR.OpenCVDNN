#include "../src/pdf/pdfium_adapter.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pdf_adapter_test <pdf>\n";
        return 2;
    }
    if (!lw::ppocr::pdf::IsAvailable()) {
        const char* configured = std::getenv("LW_PPOCR_PDFIUM_LIBRARY");
        if (configured == nullptr || *configured == '\0') {
            std::cout << "PDFium not configured; skipping PDF adapter test\n";
            return 77;
        }
        std::cerr << "PDFium was configured but is unavailable\n";
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "unable to open PDF fixture\n";
        return 1;
    }
    const std::string raw((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const std::vector<uint8_t> bytes(raw.begin(), raw.end());
    lw::ppocr::pdf::Options options;
    lw::ppocr::pdf::Document document(bytes.data(), bytes.size(), options);
    if (document.page_count() != 1) {
        std::cerr << "unexpected page count\n";
        return 1;
    }

    const auto text_page = document.process_page(0, true, false);
    if (!text_page.text_layer_usable || text_page.text.empty()) {
        std::cerr << "expected usable PDF text layer\n";
        return 1;
    }
    std::string text;
    for (const auto& item : text_page.text) text += item.text;
    if (text.find("PDFtexttest") == std::string::npos) {
        std::cerr << "unexpected extracted text: " << text << "\n";
        return 1;
    }
    for (const auto& item : text_page.text) {
        if (item.box.size() != 4) {
            std::cerr << "text item has invalid box\n";
            return 1;
        }
    }

    const auto rendered_page = document.process_page(0, false, true);
    if (rendered_page.image.empty() || rendered_page.width < 1 ||
        rendered_page.height < 1) {
        std::cerr << "expected rendered PDF page\n";
        return 1;
    }
    return 0;
}
