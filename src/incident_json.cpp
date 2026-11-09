#include "incident_json.h"

namespace tmnt {

std::string JsonQuote(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out += kHex[(c >> 4) & 0x0F];
          out += kHex[c & 0x0F];
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

}  // namespace tmnt
