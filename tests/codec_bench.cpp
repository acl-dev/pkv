#include "coder/json_record_codec.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

static double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
int main(int argc, char** argv) {
    const size_t iterations = argc > 1 ? std::stoull(argv[1]) : 100000;
    if (iterations == 0) return 1;
    std::cout << "sample,format,case,record_bytes,encode_records_per_sec,decode_and_read_records_per_sec\n";
    for (const std::string name : {"string32", "string1024", "hash8", "hash64"}) {
        for (unsigned sample = 0; sample < 5; ++sample) {
          for (unsigned order = 0; order < 2; ++order) {
            const unsigned version = (sample + order) % 2;
            pkv::json_record_codec codec(version);
            pkv::value_record record;
            if (name.compare(0, 6, "string") == 0) record.string_value.assign(std::stoul(name.substr(6)), 'x');
            else {
                record.header.kind = pkv::value_kind::hash;
                for (size_t field = 0; field < std::stoul(name.substr(4)); ++field)
                    record.hash_value["field:" + std::to_string(field)] = std::string(32, 'x');
            }
            std::string bytes, value;
            // Warm allocator/CPU state; alternate format order between samples.
            for (size_t i = 0; i < 5000; ++i) {
                std::unique_ptr<pkv::record_view> warm;
                if (codec.encode(record, bytes) != pkv::codec_status::ok ||
                    codec.decode(bytes, warm) != pkv::codec_status::ok) return 6;
            }
            auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < iterations; ++i)
                if (codec.encode(record, bytes) != pkv::codec_status::ok) return 2;
            const double encode_seconds = elapsed(start);
            std::unique_ptr<pkv::record_view> view;
            start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                if (codec.decode(bytes, view) != pkv::codec_status::ok) return 3;
                if (record.header.kind == pkv::value_kind::string) {
                    if (!view->string_value(value)) return 4;
                } else if (view->hash_field("field:" + std::to_string(i % record.hash_value.size()), value)
                           != pkv::field_status::found) return 5;
            }
            const double decode_seconds = elapsed(start);
            std::cout << sample << ',' << version << ',' << name << ',' << bytes.size() << ','
                      << iterations / encode_seconds << ',' << iterations / decode_seconds << '\n';
          }
        }
    }
}
