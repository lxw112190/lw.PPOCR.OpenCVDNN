#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lw::ppocr::http {

bool DecodeBase64(const std::string& input,
                  size_t maximum_output_bytes,
                  std::vector<uint8_t>& output,
                  std::string& error);

}  // namespace lw::ppocr::http
