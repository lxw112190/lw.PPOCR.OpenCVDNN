#include "base64.hpp"

#include <array>
#include <cctype>

namespace lw::ppocr::http {
namespace {

const std::array<int8_t, 256>& DecodeTable() {
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> value{};
        value.fill(-1);
        const char* alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int index = 0; alphabet[index] != '\0'; ++index) {
            value[static_cast<uint8_t>(alphabet[index])] =
                static_cast<int8_t>(index);
        }
        return value;
    }();
    return table;
}

}  // namespace

bool DecodeBase64(const std::string& input,
                  size_t maximum_output_bytes,
                  std::vector<uint8_t>& output,
                  std::string& error) {
    output.clear();
    error.clear();

    size_t offset = 0;
    const size_t marker = input.find(";base64,");
    if (input.rfind("data:", 0) == 0) {
        if (marker == std::string::npos) {
            error = "data URL is not Base64 encoded";
            return false;
        }
        offset = marker + 8;
    }

    const size_t encoded_length = input.size() - offset;
    if (encoded_length > ((maximum_output_bytes + 2) / 3) * 4 + 16) {
        error = "decoded image exceeds the configured size limit";
        return false;
    }

    const auto& table = DecodeTable();
    uint32_t accumulator = 0;
    int bits = 0;
    bool padding_seen = false;
    size_t symbol_count = 0;
    size_t padding_count = 0;
    output.reserve((encoded_length / 4) * 3);

    for (size_t index = offset; index < input.size(); ++index) {
        const unsigned char character =
            static_cast<unsigned char>(input[index]);
        if (std::isspace(character)) {
            continue;
        }
        if (character == '=') {
            padding_seen = true;
            ++symbol_count;
            ++padding_count;
            if (padding_count > 2) {
                error = "image_base64 has invalid padding";
                output.clear();
                return false;
            }
            continue;
        }
        if (padding_seen || table[character] < 0) {
            error = "image_base64 contains invalid characters";
            output.clear();
            return false;
        }
        ++symbol_count;

        accumulator = (accumulator << 6) |
            static_cast<uint32_t>(table[character]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output.size() >= maximum_output_bytes) {
                error = "decoded image exceeds the configured size limit";
                output.clear();
                return false;
            }
            output.push_back(static_cast<uint8_t>(
                (accumulator >> bits) & 0xFFu));
        }
    }

    if (output.empty()) {
        error = "image_base64 is empty";
        return false;
    }
    if (symbol_count % 4 != 0 ||
        bits == 6 ||
        (bits > 0 && (accumulator & ((1u << bits) - 1u)) != 0) ||
        (padding_count == 1 && bits != 2) ||
        (padding_count == 2 && bits != 4)) {
        error = "image_base64 has invalid padding";
        output.clear();
        return false;
    }
    return true;
}

}  // namespace lw::ppocr::http
