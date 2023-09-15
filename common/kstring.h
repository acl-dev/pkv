#pragma once

namespace pkv {

class kstring {
public:
    // Default constructor
    kstring() {}

    // Constructor with a C-style string
    kstring(const char* data) {
        copy(data, strlen(data));
    }

    // Constructor with a C-style string and its length
    kstring(const char* data, size_t len) {
        copy(data, len);
    }

    // Copy constructor
    kstring(const kstring& data) {
        copy(data.c_str(), data.size());
    }

    // Constructor with a std::string
    kstring(const std::string& data) {
        copy(data.c_str(), data.size());
    }

    // Move constructor
    kstring(kstring&& data) {
        if (buff_) {
            free(buff_);
        }

        buff_ = data.buff_;
        size_ = data.size_;
        dlen_ = data.dlen_;
        data.buff_ = nullptr;
        data.size_ = 0;
        data.dlen_ = 0;
    }

    // Destructor
    ~kstring() {
        if (buff_) {
            free(buff_);
        }
    }

    // Copy assignment operator with a C-style string
    kstring& operator=(const char* s) {
        copy(s, strlen(s));
        return *this;
    }

    // Copy assignment operator with a kstring
    kstring& operator=(const kstring& s) {
        copy(s.c_str(), s.size());
        return *this;
    }

    // Copy assignment operator with a std::string
    kstring& operator=(const std::string& s) {
        copy(s.c_str(), s.size());
        return *this;
    }

    // Equality operator with a kstring
    bool operator==(const kstring& s) const {
        return dlen_ == s.dlen_ && memcmp(buff_, s.buff_, dlen_) == 0;
    }

    // Equality operator with a std::string
    bool operator==(const std::string& s) const {
        return false;
    }

    // Less than operator with a kstring
    bool operator<(const kstring& s) const {
        size_t nLeft = dlen_;
        size_t nRight = s.dlen_;
        size_t n = nLeft > nRight ? nRight : nLeft;
        int   ret = memcmp(buff_, s.buff_, n);
        if (ret < 0) {
            return true;
        } else if (ret > 0) {
            return false;
        }
        if (nLeft < nRight) {
            return true;
        }
        return false;
    }

    // Returns a pointer to the underlying C-style string
    const char* c_str() const {
        return buff_;
    }

    // Returns the length of the string
    size_t size() const {
        return dlen_;
    }

    // Returns the length of the string
    size_t length() const {
        return dlen_;
    }

    // Clears the string
    void clear() {
        dlen_ = 0;
    }

    // Returns the hash value of the string
    size_t hash() const {
        return (size_t) acl_hash_func5(buff_, dlen_);
    }

private:
    // Copies the given data to the internal buffer
    void copy(const char* data, size_t len) {
        if (size_ < len + 1) {
            size_ = len + 1;
            if (buff_) {
                free(buff_);
            }
            buff_ = (char*) malloc(size_);
        }

        dlen_ = len;
        memcpy(buff_, data, dlen_);
        buff_[dlen_] = 0;
    }

private:
    char*    buff_ = nullptr; // Pointer to the internal buffer
    unsigned size_ = 0; // Size of the internal buffer
    unsigned dlen_ = 0; // Length of the string
};

} // namespace pkv

namespace std {

// Hash function specialization for kstring
template <>
struct hash<pkv::kstring> {
    size_t operator()(const pkv::kstring& key) const {
        return key.hash();
    }
};

} // namespace std 
