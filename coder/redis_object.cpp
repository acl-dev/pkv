//
// Created by shuxin 　　zheng on 2023/7/19.
//

#include "stdafx.h"
#include <climits>
#include <cstring>
#include <limits>
#include "redis_ocache.h"
#include "redis_object.h"

namespace pkv {

#if 0
#define EMPTY(x) ((x).size() == 0)
#else
#define EMPTY(x) ((x).empty())
#endif

#if 0
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)       (x)
#define UNLIKELY(x)     (x)
#endif

#define CHECK(x) { if ((x) == nullptr) (x) = new std::vector<redis_object*>; }

redis_object::redis_object(redis_ocache& cache)
: parent_(this)
, cache_(cache)
, objs_(nullptr)
{
}

redis_object::~redis_object() {
    reset();
    delete objs_;
}

void redis_object::destroy() {
    delete this;
}

void redis_object::set_parent(redis_object* parent) {
    if (parent) {
        parent_ = parent;
        depth_ = parent->depth_ + 1;
    }
}

void redis_object::recycle_children() {
    if (obj_) {
        cache_.put(obj_);
        obj_ = nullptr;
    }
    if (objs_) {
        for (auto obj : *objs_) {
            cache_.put(obj);
        }
        objs_->clear();
    }
}

const char* redis_object::get_cmd() const {
    if (type_ == REDIS_OBJ_STRING) {
        return buf_.c_str();
    }

    if (objs_ == nullptr) {
        return nullptr;
    }

    if (UNLIKELY(EMPTY(*objs_) || type_ != REDIS_OBJ_ARRAY)) {
        return nullptr;
    }

    return (*objs_)[0]->get_cmd();
}

const char* redis_object::get_str() const {
    if (type_ == REDIS_OBJ_STRING) {
        return buf_.c_str();
    }

    logger_warn("Not STRING type, type=%d", (int) type_);
    return nullptr;
}

const char* redis_object::operator[](size_t i) const {
    if (objs_ == nullptr) {
        return nullptr;
    }

    if (i >= objs_->size()) {
        return nullptr;
    }

    return (*objs_)[i]->get_str();
}

const std::string* redis_object::argument(size_t i) const {
    if (!objs_ || i >= objs_->size() || (*objs_)[i]->get_type() != REDIS_OBJ_STRING) {
        return nullptr;
    }
    return &(*objs_)[i]->get_buf();
}

struct status_machine {
    /* 状态码 */
    int status;

    /* 状态机处理函数 */
    const char* (redis_object::*func) (const char*, size_t&);
};

static struct status_machine status_tab[] = {
    { redis_s_begin,    &redis_object::parse_begin      },
    { redis_s_status,   &redis_object::parse_status     },
    { redis_s_error,    &redis_object::parse_error      },
    { redis_s_number,   &redis_object::parse_number     },
    { redis_s_strlen,   &redis_object::parse_strlen     },
    { redis_s_string,   &redis_object::parse_string     },
    { redis_s_strend,   &redis_object::parse_strend     },
    { redis_s_arlen,    &redis_object::parse_arlen      },
    { redis_s_array,    &redis_object::parse_array      },
    { redis_s_cmdline,  &redis_object::parse_cmdline    },
};

const char* redis_object::update(const char *data, size_t& len) {
    if (failed() || finish())  {
        return data;
    }

    if (obj_) {
        return parse_object(data, len);
    }

    // A complete, single-digit top-level array header can enter the
    // existing child parser without dispatching the two header states.
    if (status_ == redis_s_begin && depth_ == 0 && len >= 4 &&
        data[0] == '*' && data[1] >= '1' && data[1] <= '9' &&
        data[2] == '\r' && data[3] == '\n') {
        cnt_ = data[1] - '0';
        len -= 4;
        type_ = REDIS_OBJ_ARRAY;
        status_ = redis_s_array;
        obj_ = cache_.get();
        obj_->set_parent(this);
        return parse_object(data + 4, len);
    }

    while (len > 0) {
        //printf(">>>parse one: %s<<<\n", data);
        //printf(">>parse one<<<\n");
        data  = (this->*(status_tab[status_].func))(data, len);
        if (status_ == redis_s_null || status_ == redis_s_finish) {
            break;
        }
    }

    return data;
}

namespace {
// Probe without changing parser state. Any incomplete or unusual frame uses
// the existing streaming parser, including its exact error consumption.
size_t complete_bulk_body(const char* data, size_t len, unsigned& value) {
    if (len < 6 || data[0] != '$') return 0;
    size_t at = 1;
    value = 0;
    while (at < len && at <= 9) {
        const unsigned digit = static_cast<unsigned char>(data[at]) - '0';
        if (digit > 9) break;
        value = value * 10 + digit; ++at;
    }
    if (at == 1 || len - at < 2 || data[at] != '\r' ||
        data[at + 1] != '\n' || value > 512U * 1024 * 1024) return 0;
    const auto header = at + 2;
    if (len - header < size_t(value) + 2 ||
        data[header + value] != '\r' ||
        data[header + value + 1] != '\n') return 0;
    return header + value;
}
} // namespace

const char* redis_object::parse_object(const char* data, size_t& len) {
    assert(cnt_ > 0);
    assert(obj_);

    while (len > 0) {
        unsigned value = 0;
        const auto body = obj_->status_ == redis_s_begin &&
            obj_->depth_ <= 128 ? complete_bulk_body(data, len, value) : 0;
        if (body) {
            // Match the streaming allocation-failure state: header and body
            // are consumed before the string allocation, CRLF only after it.
            obj_->cnt_ = static_cast<int>(value);
            obj_->type_ = REDIS_OBJ_STRING;
            obj_->status_ = redis_s_string;
            len -= body;
            const auto* payload = data + body - value;
            if (value <= obj_->buf_.capacity()) {
                obj_->buf_.assign(payload, value);
            } else {
                std::string bytes(payload, value);
                obj_->buf_.swap(bytes);
            }
            data += body + 2; len -= 2;
            obj_->strend_pos_ = 2;
            obj_->status_ = redis_s_finish;
        } else data = obj_->update(data, len);
        if (obj_->failed()) {
            status_ = redis_s_null;
            return data;
        }
        if (!obj_->finish()) return data;
        CHECK(objs_);
        // Once half of a wide array has arrived, the last growth can use
        // its declared count. A header alone never reserves this space.
        if (objs_->size() >= 64 && objs_->size() == objs_->capacity() &&
            static_cast<size_t>(cnt_) - objs_->size() <= objs_->size())
            objs_->reserve(static_cast<size_t>(cnt_));
        objs_->push_back(obj_);
        obj_ = nullptr;
        if (objs_->size() == static_cast<size_t>(cnt_)) {
            cnt_ = 0;
            status_ = redis_s_finish;
            return data;
        }
        obj_ = cache_.get();
        obj_->set_parent(this);
    }
    return data;
}

const char* redis_object::parse_begin(const char* data, size_t& len) {
    if (len == 0) {
        return data;
    }

    if (depth_ > 128) {
        status_ = redis_s_null;
        return data;
    }

    switch (*data) {
    case ':':	// INTEGER
        status_ = redis_s_number;
        break;
    case '+':	// STATUS
        status_ = redis_s_status;
        break;
    case '-':	// ERROR
        status_ = redis_s_error;
        break;
    case '$':	// STRING
        status_ = redis_s_strlen;
        break;
    case '*':	// ARRAY
        status_ = redis_s_arlen;
        break;
    default:	// CMDLINE
        status_ = redis_s_cmdline;
        return data;
    }

    len--;
    data++;
    return data;
}

const char* redis_object::parse_status(const char* data, size_t& len) {
    bool found = false;
    data = get_line(data, len, buf_, found);
    if (!found) {
        assert(len == 0);
        return data;
    }

    if (failed()) return data;

    type_ = REDIS_OBJ_STATUS;
    status_ = redis_s_finish;
    return data;
}

const char* redis_object::parse_error(const char* data, size_t& len) {
    bool found = false;
    data = get_line(data, len, buf_, found);
    if (!found) {
        assert(len == 0);
        return data;
    }

    if (failed()) return data;

    type_ = REDIS_OBJ_ERROR;
    status_ = redis_s_finish;
    return data;
}

const char* redis_object::parse_number(const char* data, size_t& len) {
    bool found = false;
    data = get_line(data, len, buf_, found);
    if (!found) {
        assert(len == 0);
        return data;
    }

    if (UNLIKELY(EMPTY(buf_))) {
        status_ = redis_s_null;
        return data;
    }

    if (failed()) return data;
    size_t i = (buf_[0] == '-' || buf_[0] == '+') ? 1 : 0;
    const unsigned long long limit = buf_[0] == '-'
        ? static_cast<unsigned long long>(LLONG_MAX) + 1 : LLONG_MAX;
    unsigned long long number = 0;
    if (i == buf_.size()) {
        status_ = redis_s_null;
        return data;
    }
    for (; i < buf_.size(); ++i) {
        const unsigned char ch = buf_[i];
        if (ch < '0' || ch > '9' || number > (limit - (ch - '0')) / 10) {
            status_ = redis_s_null;
            return data;
        }
        number = number * 10 + (ch - '0');
    }
    type_ = REDIS_OBJ_INTEGER;
    status_ = redis_s_finish;
    return data;
}

const char* redis_object::parse_strlen(const char* data, size_t& len) {
    bool found = false;
    cnt_ = 0;
    data = get_length(data, len, cnt_, found);
    if (status_ == redis_s_null || !found) {
        return data;
    }

    if (cnt_ == -1) {
        type_ = REDIS_OBJ_NULL_STRING;
        status_ = redis_s_finish;
        return data;
    }
    if (cnt_ > 512 * 1024 * 1024) {
        status_ = redis_s_null;
        return data;
    }

    type_ = REDIS_OBJ_STRING;
    status_ = redis_s_string;

    return parse_string(data, len);
}

const char* redis_object::parse_string(const char* data, size_t& len) {
    if (buf_.size() < static_cast<size_t>(cnt_)) {
        data = get_data(data, len, static_cast<size_t>(cnt_));
    }
    if (buf_.size() == (size_t) cnt_) {
        status_ = redis_s_strend;
        return parse_strend(data, len);
    }

    return data;
}

const char* redis_object::parse_strend(const char* data, size_t& len) {
    static const char ending[] = "\r\n";
    while (len > 0 && strend_pos_ < 2) {
        --len;
        if (*data++ != ending[strend_pos_++]) {
            status_ = redis_s_null;
            return data;
        }
    }
    if (strend_pos_ == 2) status_ = redis_s_finish;
    return data;
}

const char* redis_object::parse_arlen(const char* data, size_t& len) {
    bool found = false;
    cnt_ = 0;
    data = get_length(data, len, cnt_, found);
    if (status_ == redis_s_null || !found) {
        return data;
    }

    if (cnt_ <= 0) {
        type_ = cnt_ == -1 ? REDIS_OBJ_NULL_ARRAY : REDIS_OBJ_ARRAY;
        status_ = redis_s_finish;
        return data;
    }
    if (cnt_ > 1024 * 1024) {
        status_ = redis_s_null;
        return data;
    }

    type_ = REDIS_OBJ_ARRAY;
    status_ = redis_s_array;

    obj_ = cache_.get();
    obj_->set_parent(this);

    return parse_array(data, len);
}

const char* redis_object::parse_array(const char* data, size_t& len) {
    assert(obj_ != nullptr);

    return parse_object(data, len);
}

const char* redis_object::parse_cmdline(const char *data, size_t &len) {
    bool found = false;
    data = get_line(data, len, buf_, found);
    if (!found) {
        assert(len == 0);
	return data;
    }

    if (failed()) return data;

    auto append = [&](const char* word, size_t size) {
        auto& obj = create_child();
        obj.buf_.assign(word, size);
        obj.type_ = REDIS_OBJ_STRING;
    };
    static const char special[] = {'\'', '"', '\\', '\0'};
    if (buf_.find_first_of(special, 0, sizeof(special)) == std::string::npos) {
        size_t pos = 0;
        while (pos < buf_.size()) {
            while (pos < buf_.size() &&
                (buf_[pos] == ' ' || buf_[pos] == '\t')) ++pos;
            const size_t start = pos;
            while (pos < buf_.size() &&
                buf_[pos] != ' ' && buf_[pos] != '\t') ++pos;
            if (pos != start) append(buf_.data() + start, pos - start);
        }
    } else {
        // Keep ACL's quote/escape and C-string semantics, owning its result
        // even when creating a child or copying an argument throws.
        std::unique_ptr<ACL_ARGV, decltype(&acl_argv_free)> words(
            acl_argv_quote_split(buf_.c_str(), " \t"), &acl_argv_free);
        for (int i = 0; i < words->argc; ++i)
            append(words->argv[i], std::strlen(words->argv[i]));
    }

    type_ = REDIS_OBJ_ARRAY;
    status_ = redis_s_finish;
    return data;
}
const char* redis_object::get_data(const char* data, size_t& len, size_t want) {
    size_t n = buf_.size();
    assert(n < want);

    want -= n;

#if 1
    if (want > len) {
        want = len;
	len = 0;
    } else {
        len -= want;
    }

    buf_.append(data, want);
    data += want;
#else
    for (size_t i = 0; i < want && len > 0; i++) {
        buf_.push_back(*data++);
        len--;
    }
#endif

    return data;
}

const char* redis_object::get_length(const char* data, size_t& len,
      int& res, bool& found) {
    // Complete short positive headers need no temporary string. Do not
    // change state until CRLF is present; fragmented/long/negative headers
    // retain the strict line parser below. Nine digits fit safely in int.
    if (buf_.empty() && !line_cr_) {
        size_t digits = 0;
        unsigned value = 0;
        while (digits < len && digits < 9) {
            const unsigned digit =
                static_cast<unsigned char>(data[digits]) - '0';
            if (digit > 9) break;
            value = value * 10 + digit;
            ++digits;
        }
        if (digits && len - digits >= 2 && data[digits] == '\r' &&
            data[digits + 1] == '\n') {
            res = static_cast<int>(value);
            found = true;
            len -= digits + 2;
            return data + digits + 2;
        }
    }
    data = get_line(data, len, buf_, found);
    if (!found) {
        assert(len == 0);
        return data;
    }

    if (UNLIKELY(EMPTY(buf_))) {
        status_ = redis_s_null;
        return data;
    }

    if (failed()) return data;
    if (buf_ == "-1") {
        res = -1;
    } else {
        res = 0;
        for (unsigned char ch : buf_) {
            if (ch < '0' || ch > '9' || res > (INT_MAX - (ch - '0')) / 10) {
                status_ = redis_s_null;
                return data;
            }
            res = res * 10 + (ch - '0');
        }
    }
    buf_.clear();
    return data;
}

const char* redis_object::get_line(const char* data, size_t& len,
      std::string& buf, bool& found) {
    while (len > 0) {
        const char ch = *data++;
        --len;
        if (line_cr_) {
            line_cr_ = false;
            found = true;
            if (ch != '\n') status_ = redis_s_null;
            return data;
        }
        if (ch == '\r') {
            line_cr_ = true;
        } else if (ch == '\n') {
            found = true;
            if (status_ != redis_s_cmdline) status_ = redis_s_null;
            return data;
        } else {
            buf.push_back(ch);
            if (buf.size() > 64 * 1024) {
                status_ = redis_s_null;
                found = true;
                return data;
            }
        }
    }
    return data;
}

bool redis_object::to_string(std::string& out, bool one_line) const {
//#define USE_UNIX_CRLF
#ifdef USE_UNIX_CRLF
#define CRLF    "\n"
#else
#define CRLF    "\r\n"
#endif

    if (objs_ && !EMPTY(*objs_)) {
        if (!one_line) {
            out.append("*").append(std::to_string(objs_->size())).append(CRLF);
        }

        for (const auto& obj : *objs_) {
            if (!obj->to_string(out, one_line)) {
                return false;
            }
        }
    }

    //printf(">>>type-%d\n", (int) type_);
    //assert(UNLIKELY(!EMPTY(buf_)));

    switch (type_) {
    case REDIS_OBJ_STATUS:
        out.append("+").append(buf_.c_str(), buf_.size()).append(CRLF);
        break;
    case REDIS_OBJ_ERROR:
        out.append("-").append(buf_.c_str(), buf_.size()).append(CRLF);
        break;
    case REDIS_OBJ_INTEGER:
        out.append(":").append(buf_.c_str(), buf_.size()).append(CRLF);
        break;
    case REDIS_OBJ_STRING:
        if (one_line) {
            if (EMPTY(out)) {
                out += " ";
            }
            out.append(buf_.c_str(), buf_.size()).append(CRLF);
        } else {
            char header[std::numeric_limits<size_t>::digits10 + 2 + sizeof(CRLF) - 1];
            auto* const end = header + sizeof(header);
            auto* next = end - (sizeof(CRLF) - 1);
            std::memcpy(next, CRLF, sizeof(CRLF) - 1);
            auto length = buf_.size();
            do {
                *--next = static_cast<char>('0' + length % 10);
                length /= 10;
            } while (length);
            *--next = '$';
            out.append(next, end - next).append(buf_).append(CRLF);
        }
        break;
    case REDIS_OBJ_ARRAY:
        if (!objs_ || objs_->empty()) out.append("*0").append(CRLF);
        break;
    case REDIS_OBJ_NULL_STRING:
        out.append("$-1").append(CRLF);
        break;
    case REDIS_OBJ_NULL_ARRAY:
        out.append("*-1").append(CRLF);
        break;
    case REDIS_OBJ_EMPTY:
        out.append("*0").append(CRLF);
        break;
    default:
        break;
    }

    return true;
}

redis_object& redis_object::set_status(const std::string& data,
      bool return_parent) {
    type_ = REDIS_OBJ_STATUS;
    buf_ = data;
    return return_parent ? *parent_ : *this;
}

redis_object& redis_object::set_error(const std::string& data,
      bool return_parent) {
    type_ = REDIS_OBJ_ERROR;
    buf_ = data;
    return return_parent ? *parent_ : *this;
}

redis_object& redis_object::set_number(long long n, bool return_parent) {
    type_ = REDIS_OBJ_INTEGER;
    buf_ = std::to_string(n);
    return return_parent ? *parent_ : *this;
}

redis_object& redis_object::set_string(const std::string &data,
      bool return_parent) {
    type_ = REDIS_OBJ_STRING;
    buf_ = data;
    return return_parent ? *parent_ : *this;
}

redis_object& redis_object::set_null(bool return_parent) {
    type_ = REDIS_OBJ_NULL_STRING;
    buf_.clear();
    return return_parent ? *parent_ : *this;
}

redis_object& redis_object::create_child() {
    redis_object* obj = cache_.get();
    obj->set_parent(this);

    try {
        CHECK(objs_);
        objs_->push_back(obj);
    } catch (...) {
        obj->destroy(); throw;
    }

    if (obj_ == nullptr) {
        // The last one is NULL.
        type_ = REDIS_OBJ_ARRAY;
    }

    cnt_ = objs_->size();
    return *obj;
}

void redis_object::set_type(pkv::redis_obj_t type) {
    type_ = type;
}

void redis_object::adopt_child(redis_object& object) {
    CHECK(objs_);
    objs_->push_back(&object);
    object.set_parent(this);
    type_ = REDIS_OBJ_ARRAY;
    cnt_ = objs_->size();
}

} // namespace pkv
