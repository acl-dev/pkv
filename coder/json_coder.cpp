//
// Created by zsx on 2023/8/19.
//

#include "stdafx.h"
#include "json_object.h"
#include "json_doc.h"
#include "json_coder.h"

namespace pkv {

json_coder::json_coder()
: finished_(false)
, result_(nullptr)
, doc_(nullptr)
, root_(nullptr)
{
}

json_coder::~json_coder() {
    if (result_) {
        free(result_);
    }

    delete doc_;
}

bool json_coder::parse(const char *data, size_t len) {
    auto doc = yyjson_read(data, len, 0);
    if (doc == nullptr) {
        return false;
    }

    doc_ = new json_doc(doc);
    auto root = yyjson_doc_get_root(doc);
    root_ = new json_object(root);
    return true;
}

void json_coder::create_writer() {
    assert(doc_ == nullptr);
    doc_ = new json_doc();
    auto mdoc = doc_->get_mut_doc();
    auto root = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, root);
    root_ = new json_object(root);
}

} // namespace pkv