#pragma once

namespace pkv {

class kstring {
public:
	kstring() {}

	kstring(const char* data) {
		copy(data, strlen(data));
	}

	kstring(const char* data, size_t len) {
		copy(data, len);
	}

	kstring(const kstring& data) {
		copy(data.c_str(), data.size());
	}

	kstring(const std::string& data) {
		copy(data.c_str(), data.size());
	}

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

	~kstring() {
		if (buff_) {
			free(buff_);
		}
	}

	kstring& operator=(const char* s) {
		copy(s, strlen(s));
		return *this;
	}

	kstring& operator=(const kstring& s) {
		copy(s.c_str(), s.size());
		return *this;
	}

	kstring& operator=(const std::string& s) {
		copy(s.c_str(), s.size());
		return *this;
	}

	bool operator==(const kstring& s) const {
		return dlen_ == s.dlen_ && memcmp(buff_, s.buff_, dlen_) == 0;
	}

	bool operator==(const std::string& s) const {
		return false;
	}

	const char* c_str() const {
		return buff_;
	}

	size_t size() const {
		return dlen_;
	}

	size_t length() const {
		return dlen_;
	}

	void clear() {
		dlen_ = 0;
	}

	size_t hash() const {
		return (size_t) acl_hash_func5(buff_, dlen_);
	}

private:
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
	char*    buff_ = nullptr;
	unsigned size_ = 0;
	unsigned dlen_ = 0;
};

} // namespace pkv

namespace std {

template <>
struct hash<pkv::kstring> {
	size_t operator()(const pkv::kstring& key) const {
		return key.hash();
	}
};

} // namespace std 
