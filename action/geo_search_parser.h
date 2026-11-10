#pragma once
#include "coder/redis_object.h"
#include "common/geo_search.h"
#include "common/geo_radius.h"

namespace pkv {
class geo_search_parser {
public:
    geo_search_parser(const redis_object& object, const char* command);
    bool parse(const geo::search_source& source, geo::search_options& options);
    const std::string& error() const { return error_; }
private:
    const redis_object& object_;
    const bool store_;
    const geo_radius_mode radius_;
    const std::string* destination_ = nullptr;
    std::string error_;
    bool fail(std::string text) { error_ = std::move(text); return false; }
    bool coordinates(size_t first, geo::point& out);
    bool dimensions(size_t first, bool box, geo::shape& out);
    bool member(const geo::search_source& source, const std::string& name, geo::point& out);
};
} // namespace pkv
