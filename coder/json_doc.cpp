//
// Created by zsx on 2023/8/19.
//

#include "stdafx.h"
#include "json_doc.h"

namespace pkv {

json_doc::json_doc(yyjson_doc *doc) : r_doc_(doc), w_doc_(nullptr) {}

json_doc::json_doc() : r_doc_(nullptr) {
    w_doc_ = yyjson_mut_doc_new(nullptr);
}

json_doc::~json_doc() {
    if (w_doc_) {
        yyjson_mut_doc_free(w_doc_);
    }

    if (r_doc_) {
        yyjson_doc_free(r_doc_);
    }
}

} // namespace pkv