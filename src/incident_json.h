#pragma once

#include <string>
#include <string_view>

namespace tmnt {

// A JSON string literal, including the quotes, with every control byte escaped.
std::string JsonQuote(std::string_view text);

}  // namespace tmnt
