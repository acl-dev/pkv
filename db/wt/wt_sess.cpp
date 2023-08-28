//
// Created by shuxin ¡¡¡¡zheng on 2023/7/27.
//

#include "stdafx.h"

#ifdef HAS_WT

#include "wt.h"
#include "wt_sess.h"

namespace pkv {

wt_sess::wt_sess(wt& db) : db_(db), n_(0), sess_(nullptr), curs_(nullptr) {}

wt_sess::~wt_sess() = default;

bool wt_sess::open() {
    auto db = db_.get_db();
    int ret = db->open_session(db, nullptr, nullptr, &sess_);
    if (ret != 0) {
        logger_error("open session error %d", ret);
        return false;
    }

    const char* table_name = "table:access";

    std::string table_conf;
    table_conf += "split_pct=90,leaf_item_max=1KB";
    table_conf += ",type=lsm,internal_page_max=16KB,leaf_page_max=16KB";
    //table_conf += ",lsm=(chunk_size=4MB,bloom_config=(leaf_page_max=4MB))";
    //table_conf += ",block_compressor=snappy";
    table_conf += ",key_format=S,value_format=S";

    ret = sess_->create(sess_, table_name, table_conf.c_str());
    if (ret != 0) {
        logger_error("create session error %d", ret);
        return false;
    }

    ret = sess_->open_cursor(sess_, table_name, nullptr, "overwrite=true", &curs_);
    if (ret != 0) {
        logger_error("open cursor error %d", ret);
        return false;
    }

    return true;
}

bool wt_sess::add(const std::string &key, const std::string &value) {
    assert(curs_);
    if (n_ == 0) {
        //sess_->begin_transaction(sess_, NULL);
    }

    curs_->set_key(curs_, key.c_str());
    curs_->set_value(curs_, value.c_str());
    int ret = curs_->insert(curs_);
    if (ret != 0) {
        logger_error("insert %s %s error=%d", key.c_str(), value.c_str(), ret);
        curs_->reset(curs_);
        //sess_->commit_transaction(sess_, NULL);
        return false;
    }
    curs_->reset(curs_);
    if (++n_ >= 1000) {
        //sess_->commit_transaction(sess_, NULL);
        n_ = 0;
    }
    return true;
}

bool wt_sess::get(const std::string &key, std::string &value) {
    assert(curs_);
    curs_->set_key(curs_, key.c_str());
    int ret = curs_->search(curs_);
    if (ret != 0) {
        if (ret != WT_NOTFOUND) {
            logger("search key=%s error=%d", key.c_str(), ret);
        }
        curs_->reset(curs_);
        return false;
    }

    const char* v;
    ret = curs_->get_value(curs_, &v);
    if (ret != 0) {
        logger_error("get_value key=%s errort=%d", key.c_str(), ret);
        curs_->reset(curs_);
        return false;
    }

    value = v;
    curs_->reset(curs_);
    return true;
}

bool wt_sess::del(const std::string &key) {
    assert(curs_);
    curs_->set_key(curs_, key.c_str());
    int ret = curs_->remove(curs_);
    if (ret != 0 && ret != WT_NOTFOUND) {
        logger_error("remove key=%s error=%d", key.c_str(), ret);
        curs_->reset(curs_);
        return false;
    }

    curs_->reset(curs_);
    return true;
}

} // namespace pkv

#endif // HAS_WT
