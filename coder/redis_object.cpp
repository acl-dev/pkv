//
// Created by shuxin 　　zheng on 2023/7/19.
//

#include "stdafx.h"
#include <climits>
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

void redis_object::reset() {
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

    response_kind_ = response_kind::plain;
    response_protocol_ = 0;
    status_ = redis_s_begin;
    type_   = REDIS_OBJ_UNKOWN;
    parent_ = this;
    obj_    = nullptr;
    cnt_    = 0;
    depth_ = 0;
    line_cr_ = false;
    strend_pos_ = 0;

    buf_.clear();
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

const char* redis_object::parse_object(const char* data, size_t& len) {
    assert(cnt_ > 0);
    assert(obj_);

    while (len > 0) {
        data = obj_->update(data, len);
        if (obj_->failed()) {
            status_ = redis_s_null;
            return data;
        }
        if (!obj_->finish()) return data;
        CHECK(objs_);
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

    acl::string buf(buf_.c_str());
    auto& tokens = buf.split2(" \t", true);
    for (auto& it : tokens) {
        auto& obj = create_child();
        obj.buf_ = it.c_str();
        obj.type_ = REDIS_OBJ_STRING;
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
            out.append("$").append(std::to_string(buf_.size())).append(CRLF)
                .append(buf_.c_str(), buf_.size()).append(CRLF);
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
