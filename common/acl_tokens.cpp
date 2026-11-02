#include "acl_internal.h"
#include <cctype>

namespace pkv {
namespace {
int hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
char escaped(char ch) {
    switch (ch) {
    case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t';
    case 'b': return '\b'; case 'a': return '\a'; default: return ch;
    }
}
} // namespace
bool acl_split(const std::string& text, std::vector<std::string>& out) {
    out.clear();
    size_t offset = 0;
    while (offset < text.size()) {
        while (offset < text.size() && std::isspace(
            static_cast<unsigned char>(text[offset]))) ++offset;
        if (offset == text.size()) break;
        std::string word;
        char quote = 0;
        bool done = false;
        while (!done && offset < text.size()) {
            char ch = text[offset++];
            if (quote) {
                if (ch == quote) {
                    if (offset < text.size() && !std::isspace(
                        static_cast<unsigned char>(text[offset]))) return false;
                    quote = 0; done = true;
                } else if (ch == '\\' && offset < text.size() &&
                    (quote == '"' || text[offset] == '\'')) {
                    ch = text[offset++];
                    if (quote == '"' && ch == 'x' && offset + 1 < text.size()
                        && hex(text[offset]) >= 0 &&
                        hex(text[offset + 1]) >= 0) {
                        word += static_cast<char>(hex(text[offset]) * 16 +
                            hex(text[offset + 1])); offset += 2;
                    } else word += quote == '"' ? escaped(ch) : ch;
                } else word += ch;
            } else if (std::isspace(static_cast<unsigned char>(ch)))
                done = true;
            else if (ch == '"' || ch == '\'') quote = ch;
            else word += ch;
        }
        if (quote) return false;
        out.push_back(std::move(word));
    }
    return true;
}
std::string acl_quote(const std::string& text) {
    bool quote = text.empty();
    for (unsigned char ch : text)
        quote = quote || ch <= ' ' || ch == 127 || ch == '"' || ch == '\'' ||
            ch == '\\';
    if (!quote) return text;
    static const char digits[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < ' ' || ch == 127) {
            out += "\\x"; out += digits[ch >> 4]; out += digits[ch & 15];
        } else out += static_cast<char>(ch);
    }
    return out + '"';
}
} // namespace pkv
