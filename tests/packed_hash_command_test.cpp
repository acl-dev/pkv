#include "stdafx.h"
#include "coder/packed_hash.h"
#include "coder/compact_hash.h"
#include "dao/paged_hash.h"
#include "dao/json/json_key.h"
#include "dao/json/json_scan.h"
#include "db/mdb/mdb.h"
#include "db/packed_hash_store.h"
#ifdef HAS_TBB
#include "db/mdb/mdb_tbb.h"
#endif
#include <cstdio>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"packed command failed line %d\n",__LINE__);std::abort();}}while(0)
using namespace pkv;
using fields=std::map<std::string,std::string>;
class native_only_mdb : public mdb {
public:
    explicit native_only_mdb(size_t count) : mdb(count) {}
protected:
    bool dbinline_strings() const override { return false; }
};
static fields fixture(unsigned count){fields out;for(unsigned i=0;i<count;++i){char field[40];std::snprintf(field,sizeof(field),"field:%013u",i);out[field]="val";}return out;}
static packed_hash::root grouped(shared_db& db,const std::string& key,unsigned count){std::string bytes;packed_hash::root root;REQUIRE(db->get(key,bytes)&&packed_hash::matches(bytes)&&packed_hash::decode_root(bytes,root)==map_page_status::ok&&root.elements==count);return root;}
static void verify(dao::paged_hash& hash,shared_db& db,const std::string& key,const fields& expected){dao::paged_hash::field_list values;REQUIRE(hash.hgetall(db,key,true,values));REQUIRE((fields(values.begin(),values.end())==expected));long long length;REQUIRE(hash.hlen(db,key,length)&&length==static_cast<long long>(expected.size()));}
template<class Backend>static void run(){shared_db db=std::make_shared<Backend>(16),destination=std::make_shared<Backend>(16);REQUIRE(db->open("")&&destination->open(""));dao::paged_hash hash;dao::json_key keys;auto expected=fixture(512);REQUIRE(hash.hmset(db,"hash",expected)==512);grouped(db,"hash",512);std::string type;REQUIRE(keys.type(db,"hash",type)&&type=="hash");const long long deadline=4102444800123LL;REQUIRE(keys.set_expiry(db,"hash",deadline,0));REQUIRE(grouped(db,"hash",512).expires_at_ms==deadline);verify(hash,db,"hash",expected);
 std::vector<watch_token> watch;REQUIRE(db->watch({"hash"},watch)==db::batch_status::ok&&watch[0]->valid(unix_time_ms()));bool added;REQUIRE(hash.hsetnx(db,"hash",expected.begin()->first,std::string(4000,'x'),added)&&!added&&watch[0]->valid(unix_time_ms()));REQUIRE(hash.hset(db,"hash",expected.begin()->first,"val",added)&&!added&&!watch[0]->valid(unix_time_ms()));
 const auto number=expected.begin()->first;REQUIRE(hash.hset(db,"hash",number,"1",added)&&!added);long long result;REQUIRE(hash.hincrby(db,"hash",number,2,result)&&result==3);expected[number]="3";std::string value;REQUIRE(hash.hincrbyfloat(db,"hash",number,"0.5",value)&&value=="3.5");expected[number]="3.5";verify(hash,db,"hash",expected);
 bool copied;REQUIRE(keys.copy(db,"hash",destination,"copy",false,copied)&&copied);grouped(destination,"copy",512);verify(hash,destination,"copy",expected);long long expiry;REQUIRE(keys.expiry_time(destination,"copy",expiry)&&expiry==deadline);bool renamed;REQUIRE(keys.rename(destination,"copy","renamed",false,renamed)&&renamed);grouped(destination,"renamed",512);verify(hash,destination,"renamed",expected);
 // A destination without compact support must receive native Map pages,
 // including when the source is oneTBB and the destination is plain MDB.
 shared_db native_destination=std::make_shared<native_only_mdb>(16);
 REQUIRE(native_destination->open("")&&!native_destination->supports_compact_hashes());
 REQUIRE(keys.copy(db,"hash",native_destination,"native-copy",false,copied)&&copied);
 std::string copied_bytes;map_root native_root;
 REQUIRE(native_destination->get("native-copy",copied_bytes)&&!packed_hash::matches(copied_bytes));
 REQUIRE(map_page_codec::decode_root(copied_bytes,native_root)==map_page_status::ok);
 REQUIRE(native_root.elements==512&&native_root.expires_at_ms==deadline);
 verify(hash,native_destination,"native-copy",expected);
 REQUIRE(hash.hset(native_destination,"native-copy",number,std::string(4000,'z'),added)&&!added);
 auto native_expected=expected;native_expected[number]=std::string(4000,'z');
 verify(hash,native_destination,"native-copy",native_expected);verify(hash,db,"hash",expected);
 REQUIRE(keys.copy(db,"hash",native_destination,"native-copy",true,copied)&&copied);
 verify(hash,native_destination,"native-copy",expected);
 dao::json_scan scan;REQUIRE(scan.open_collection(db,"hash",value_kind::hash));scan_options options;options.count=11;fields scanned;unsigned rounds=0;do{scan_page page;REQUIRE(scan.collection(options,page)&&page.values.size()%2==0);for(size_t i=0;i<page.values.size();i+=2)REQUIRE(scanned.emplace(page.values[i],page.values[i+1]).second);options.cursor=page.cursor;REQUIRE(++rounds<=256);}while(options.cursor);REQUIRE(scanned==expected);
 // Incremental compact promotion must also reach the grouped format.
 auto growing=fixture(64);for(const auto& item:growing)REQUIRE(hash.hset(db,"growing",item.first,item.second,added)&&added);grouped(db,"growing",64);verify(hash,db,"growing",growing);
 // Contraction is terminal and must drop every old group while retaining TTL.
 std::vector<std::string> remove;for(auto it=expected.begin();expected.size()-remove.size()>32;++it)remove.push_back(it->first);REQUIRE(hash.hdel(db,"hash",remove)==static_cast<int>(remove.size()));for(const auto& field:remove)expected.erase(field);std::string bytes;REQUIRE(db->get("hash",bytes)&&compact_hash::matches(bytes)&&compact_hash::view(bytes).deadline()==deadline);verify(hash,db,"hash",expected);
 // Native fallback preserves data and remains native while values are large.
 auto native=fixture(129);REQUIRE(hash.hmset(db,"native",native)==129);grouped(db,"native",129);REQUIRE(keys.set_expiry(db,"native",deadline,0));REQUIRE(hash.hset(db,"native",native.begin()->first,std::string(4000,'x'),added)&&!added);native.begin()->second=std::string(4000,'x');REQUIRE(db->get("native",bytes)&&!packed_hash::matches(bytes));verify(hash,db,"native",native);REQUIRE(keys.expiry_time(db,"native",expiry)&&expiry==deadline);
}
template<class Backend> class yielding_backend : public Backend {
public:
    explicit yielding_backend(size_t count) : Backend(count) {}
    unsigned inspections = 0;
protected:
    db::lookup_status dblookup(const std::string& key,std::string& value) override {
        acl::fiber::yield(); return Backend::dblookup(key,value);
    }
    bool dbbind_page_reader(const std::string&,db::page_reader&) override { return false; }
    db::lookup_status dbinspect_page(const std::string& key,uint64_t id,
        std::string& scratch,db::page_inspection& view) override {
        ++inspections; acl::fiber::yield();
        const auto result = Backend::dbinspect_page(key,id,scratch,view);
        acl::fiber::yield(); return result;
    }
};
template<class Backend> static void suspended_contexts() {
    auto backend = std::make_shared<yielding_backend<Backend>>(16);
    shared_db database = backend; REQUIRE(database->open(""));
    unsigned completed = 0;
    for (unsigned worker = 0; worker < 8; ++worker) {
        const std::string owner = std::string(80,'k') + std::to_string(worker);
        go [&,owner] {
            dao::paged_hash hash;
            auto expected = fixture(64);
            REQUIRE(hash.hmset(database,owner,expected) == 64);
            const auto expanded = fixture(512);
            auto update = [&](page_transaction& tx) {
                map_store store(tx,owner); REQUIRE(store.open());
                // Public contexts must still own a temporary caller key.
                packed_hash_store owned(tx,std::string(owner)); REQUIRE(owned.open());
                for (const auto& item : expanded) {
                    bool added;
                    REQUIRE(store.put(item.first,item.second,false,added));
                    REQUIRE(added == !expected.count(item.first));
                    expected[item.first] = item.second;
                }
                for (const long long expiry : {4102444800123LL,-1LL,4102444800123LL}) {
                    REQUIRE(store.set_expiry(expiry));
                    std::string value; bool found;
                    REQUIRE(store.get(expanded.begin()->first,value,found) && found && value == "val");
                    REQUIRE(store.metadata().expires_at_ms == expiry);
                }
                return store.finish_hash();
            };
            REQUIRE(database->transact_page(owner,std::ref(update)) == db::batch_status::ok);
            grouped(database,owner,512); verify(hash,database,owner,expected);
            // A later context must validate again rather than trusting a cache
            // from an earlier context for this same root entry.
            auto invalid = [&](page_transaction& tx) {
                map_store first(tx,owner); REQUIRE(first.open());
                auto& bytes = tx.roots().find(owner)->value;
                bytes[5] = bytes[6] = 0;
                map_store second(tx,owner); REQUIRE(!second.open());
                REQUIRE(second.error() == map_store::status::invalid);
                return false;
            };
            REQUIRE(database->transact_page(owner,std::ref(invalid)) == db::batch_status::aborted);
            verify(hash,database,owner,expected);
            ++completed;
        };
    }
    acl::fiber::schedule(); REQUIRE(completed == 8 && backend->inspections);
}
int main(){acl::acl_cpp_init();go []{};acl::fiber::schedule();run<mdb>();suspended_contexts<mdb>();
#ifdef HAS_TBB
run<mdb_tbb>();suspended_contexts<mdb_tbb>();
#endif
std::puts("packed Hash DAO command integration passed");}
