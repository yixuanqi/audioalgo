#pragma once
#include <string>
#include <string_view>
#include <cstdint>
namespace audioalgo {
class Utf8LogDecoder {
public:
    std::string append(std::string_view bytes) { pending_.append(bytes); return decode(false); }
    std::string finish() { return decode(true); }
private:
    std::string pending_;
    std::string decode(bool final) {
        std::string out; out.reserve(pending_.size()); std::size_t position = 0;
        while (position < pending_.size()) {
            const auto lead = static_cast<unsigned char>(pending_[position]);
            if (lead < 0x80) {
                if (lead == 0) out += "\xe2\x90\x80"; else out += static_cast<char>(lead);
                ++position; continue;
            }
            const std::size_t length = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
            if (length && position + length > pending_.size() && !final) break;
            bool valid = length && position + length <= pending_.size();
            std::uint32_t code = lead & (0x7f >> length);
            for (std::size_t i = 1; valid && i < length; ++i) {
                const auto byte = static_cast<unsigned char>(pending_[position + i]);
                valid = (byte & 0xc0) == 0x80; code = (code << 6) | (byte & 0x3f);
            }
            valid = valid && code >= (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u)
                && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
            if (valid) { out.append(pending_, position, length); position += length; }
            else { out += "\xef\xbf\xbd"; ++position; }
        }
        pending_.erase(0, position); return out;
    }
};
// Keep a valid UTF-8 suffix. Return the number of displayed bytes discarded.
inline std::size_t trim_log_text(std::string& text, std::size_t maximum) {
    if (text.size() <= maximum) return 0;
    auto start = text.size() - maximum;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) ++start;
    text.erase(0, start); return start;
}
}
