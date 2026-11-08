#include "redis_object.h"
#include <cstring>
#include <cassert>
#include <stdexcept>

namespace pkv {
redis_object& redis_object::set_raw(std::string&& data) {
    type_ = REDIS_OBJ_STRING;
    buf_ = std::move(data);
    return response_as(response_kind::raw);
}

redis_object& redis_object::set_bulk_collection(
    const std::vector<std::string>& values, bool as_set) {
    return set_bulk_collection(values.data(), values.size(), as_set);
}

namespace {
template<class ValueAt>
std::string encode_bulk_array(size_t count, ValueAt value_at) {
    std::string frame;
    const auto entries = std::to_string(count);
    size_t frame_size = entries.size() + 3;
    const auto limit = frame.max_size();
    for (size_t i = 0; i < count; ++i) {
        const auto& value = value_at(i);
        size_t overhead = 6;
        for (auto length = value.size(); length >= 10; length /= 10)
            ++overhead;
        if (overhead > limit - frame_size ||
            value.size() > limit - frame_size - overhead)
            throw std::length_error("bulk collection reply too large");
        frame_size += value.size() + overhead;
    }
    frame.resize(frame_size);
    auto* next = &frame[0];
    const auto append = [&](const char* bytes, size_t length) {
        std::memcpy(next, bytes, length);
        next += length;
    };
    *next++ = '*';
    append(entries.data(), entries.size());
    append("\r\n", 2);
    for (size_t i = 0; i < count; ++i) {
        const auto& value = value_at(i);
        const auto length = value.size();
        *next++ = '$';
        if (length < 10) {
            *next++ = static_cast<char>('0' + length);
        } else if (length < 100) {
            *next++ = static_cast<char>('0' + length / 10);
            *next++ = static_cast<char>('0' + length % 10);
        } else {
            const auto digits = std::to_string(length);
            append(digits.data(), digits.size());
        }
        append("\r\n", 2);
        append(value.data(), length);
        append("\r\n", 2);
    }
    assert(next == frame.data() + frame.size());
    return frame;
}
} // namespace

redis_object& redis_object::set_bulk_collection(
    const std::string* values, size_t count, bool as_set) {
    set_raw(encode_bulk_array(count, [values](size_t i) -> const std::string& {
        return values[i];
    }));
    return as_set ? response_as(response_kind::raw_set) : *this;
}

redis_object& redis_object::set_bulk_array(const std::string* const* values,
    size_t count) {
    return set_raw(encode_bulk_array(count, [values](size_t i) -> const std::string& {
        return *values[i];
    }));
}

bool redis_object::to_resp(std::string& out, unsigned protocol) const {
    if (response_protocol_) protocol = response_protocol_;
    if (protocol != 2 && protocol != 3) return false;
    const auto count = objs_ ? objs_->size() : 0;
    if (response_kind_ == response_kind::raw || response_kind_ == response_kind::raw_set) {
        if (type_ != REDIS_OBJ_STRING || count) return false;
        if (protocol == 3 && response_kind_ == response_kind::raw_set) {
            if (buf_.empty() || buf_.front() != '*') return false;
            out += '~';
            out.append(buf_.data() + 1, buf_.size() - 1);
        } else out += buf_;
        return true;
    }
    if (count || type_ == REDIS_OBJ_ARRAY || type_ == REDIS_OBJ_EMPTY) {
        const auto kind = protocol == 3 ? response_kind_ : response_kind::plain;
        const bool map = kind == response_kind::map;
        const bool pairs = kind == response_kind::pairs;
        if ((map || pairs) && count % 2) return false;
        const char prefix = map ? '%' : kind == response_kind::set ? '~' : '*';
        out += prefix;
        out += std::to_string(map || pairs ? count / 2 : count);
        out += "\r\n";
        for (size_t i = 0; i < count; ++i) {
            if (pairs && i % 2 == 0) out += "*2\r\n";
            if (!(*objs_)[i]->to_resp(out, protocol)) return false;
        }
        return true;
    }
    // Generated signed integers fit in 20 bytes. Parsed integers may have
    // longer spellings with leading zeroes and use the existing formatter.
    const auto length = buf_.size();
    if (type_ == REDIS_OBJ_INTEGER && length <= 20) {
        char frame[23];
        frame[0] = ':';
        std::memcpy(frame + 1, buf_.data(), length);
        frame[length + 1] = '\r';
        frame[length + 2] = '\n';
        out.append(frame, length + 3);
        return true;
    }
    if (protocol == 3) {
        if (type_ == REDIS_OBJ_STRING &&
            response_kind_ == response_kind::verbatim) {
            out += '='; out += std::to_string(buf_.size() + 4);
            out += "\r\ntxt:"; out += buf_; out += "\r\n"; return true;
        }
        if (type_ == REDIS_OBJ_NULL_STRING || type_ == REDIS_OBJ_NULL_ARRAY) {
            out += "_\r\n"; return true;
        }
        if (type_ == REDIS_OBJ_STRING &&
            response_kind_ == response_kind::real) {
            out += ','; out += buf_; out += "\r\n"; return true;
        }
    }
    return to_string(out);
}

void redis_object::metadata_response(bool docs) {
    if (!objs_) return;
    if (docs) {
        response_as(response_kind::map);
        return;
    }
    if (objs_->size() != 10) return;
    for (const auto i : {2, 6, 7, 8})
        (*objs_)[i]->response_as(response_kind::set);
    auto& specs = *(*objs_)[8];
    if (specs.objs_) for (auto* spec : *specs.objs_) {
        spec->response_as(response_kind::map);
        if (!spec->objs_) continue;
        for (size_t i = 0; i + 1 < spec->objs_->size(); i += 2) {
            auto& name = *(*spec->objs_)[i];
            auto& value = *(*spec->objs_)[i + 1];
            if (name.buf_ == "flags") value.response_as(response_kind::set);
            else if (name.buf_ == "begin_search" || name.buf_ == "find_keys") {
                value.response_as(response_kind::map);
                if (value.objs_) for (size_t j = 0;
                    j + 1 < value.objs_->size(); j += 2)
                    if ((*value.objs_)[j]->buf_ == "spec")
                        (*value.objs_)[j + 1]->response_as(response_kind::map);
            }
        }
    }
    auto& children = *(*objs_)[9];
    if (!children.objs_ || children.objs_->empty())
        children.response_as(response_kind::set);
    else for (auto* child : *children.objs_) child->metadata_response(false);
}
} // namespace pkv
