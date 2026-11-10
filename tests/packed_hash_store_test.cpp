#include "stdafx.h"
#include "db/packed_hash_store.h"
#include "db/mdb/mdb.h"
#include "dao/json/json_key.h"
#ifdef HAS_TBB
#include "db/mdb/mdb_tbb.h"
#endif
#include <cstdio>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d\n",__LINE__); std::abort(); } } while(0)
static thread_local long fail_allocation = -1;
void* operator new(size_t n){if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}if(fail_allocation>0)--fail_allocation;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
#if __cplusplus>=201402L
void operator delete(void* p,size_t)noexcept{std::free(p);}
void operator delete[](void* p,size_t)noexcept{std::free(p);}
#endif
using namespace pkv;
static const std::string key="hash";
static const int64_t deadline=4102444800123LL;
static packed_hash_store::fields fixture(unsigned n) {
    packed_hash_store::fields fields;
    for(unsigned i=0;i<n;++i){char name[32];std::snprintf(name,sizeof(name),"field:%013u",i);fields[name]="val";}
    return fields;
}
static void verify(page_transaction& tx,const packed_hash_store::fields& expected,int64_t ttl,bool native) {
    if(native){map_store store(tx,key);REQUIRE(store.open());REQUIRE(store.metadata().elements==expected.size()&&store.metadata().expires_at_ms==ttl);packed_hash_store::fields all;
        REQUIRE(store.visit([&](const std::string& f,const std::string& v){return all.emplace(f,v).second;}));REQUIRE(all==expected);REQUIRE(store.finish());
    }else{packed_hash_store store(tx,key);REQUIRE(store.open());REQUIRE(store.size()==expected.size()&&store.expiry()==ttl);packed_hash_store::field_list all;REQUIRE(store.all(all));REQUIRE((packed_hash_store::fields(all.begin(),all.end())==expected));}
}
template<class Backend>static void run() {
    shared_db database=std::make_shared<Backend>(16);REQUIRE(database->open(""));
    auto expected=fixture(64);
    auto seed=[&](page_transaction& tx){bool created=false;REQUIRE(packed_hash_store::create(tx,key,expected,deadline,created)&&created);verify(tx,expected,deadline,false);return true;};
    REQUIRE(database->transact_page(key,std::ref(seed))==db::batch_status::ok);
    auto round=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());bool found,added;std::string value;
        REQUIRE(store.get(expected.begin()->first,value,found)&&found&&value=="val");
        REQUIRE(store.put(expected.begin()->first,std::string(4096,'x'),true,added)&&!added&&!store.native());
        REQUIRE(store.put(expected.begin()->first,std::string("a\0b",3),false,added)&&!added);expected.begin()->second=std::string("a\0b",3);
        uint64_t length;REQUIRE(store.length(expected.begin()->first,length,found)&&found&&length==3);
        REQUIRE(store.take(expected.begin()->first,value,found)&&found&&value==std::string("a\0b",3));auto removed=expected.begin()->first;expected.erase(expected.begin());
        value="unchanged";REQUIRE(store.take(removed,value,found)&&!found&&value=="unchanged");
        REQUIRE(store.put(removed,"new",true,added)&&added);expected[removed]="new";
        REQUIRE(store.set_expiry(-1)&&store.expiry()==-1);REQUIRE(store.set_expiry(deadline));
        packed_hash_store::field_list all;REQUIRE(store.all(all)&&packed_hash_store::fields(all.begin(),all.end())==expected);return true;};
    REQUIRE(database->transact_page(key,std::ref(round))==db::batch_status::ok);
    // Growth consumes staged pages repeatedly before a single commit.
    auto growth=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());for(const auto& item:fixture(512)){bool added;const bool existed=expected.count(item.first);REQUIRE(store.put(item.first,item.second,false,added)&&added!=existed);expected[item.first]=item.second;}
        REQUIRE(store.size()==512&&!store.native());verify(tx,expected,deadline,false);
        packed_hash_store::fields ranked;for(uint64_t rank=0;rank<store.size();++rank){std::string f,v;bool found;REQUIRE(store.select(rank,f,found,&v)&&found&&ranked.emplace(f,v).second);}REQUIRE(ranked==expected);
        std::string sentinel="unchanged";bool found;REQUIRE(store.select(store.size(),sentinel,found)&&!found&&sentinel=="unchanged");
        scan_options options;options.count=7;packed_hash_store::fields scanned;unsigned rounds=0;do{scan_page page;REQUIRE(store.scan(options,page)&&page.values.size()%2==0);for(size_t i=0;i<page.values.size();i+=2)REQUIRE(scanned.emplace(page.values[i],page.values[i+1]).second);options.cursor=page.cursor;REQUIRE(++rounds<=256);}while(options.cursor);REQUIRE(scanned==expected);
        options.cursor=0;options.has_match=true;options.pattern="*005*";options.no_values=true;std::set<std::string> names;do{scan_page page;REQUIRE(store.scan(options,page));for(const auto& f:page.values)REQUIRE(names.insert(f).second);options.cursor=page.cursor;}while(options.cursor);std::set<std::string> matches;for(const auto& item:expected)if(redis_glob_match(options.pattern,item.first))matches.insert(item.first);REQUIRE(names==matches);
        return true;};
    REQUIRE(database->transact_page(key,std::ref(growth))==db::batch_status::ok);
    // Conversion must include earlier staged writes and preserve the key TTL.
    auto convert=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());bool added;REQUIRE(store.put(expected.begin()->first,"staged",false,added)&&!added);expected.begin()->second="staged";
        auto last=std::prev(expected.end())->first;std::string large(4096,'x');REQUIRE(store.put(last,large,false,added)&&!added&&store.native());expected[last]=large;
        std::string value;bool found;REQUIRE(store.get(last,value,found)&&found&&value==large);REQUIRE(store.put("new-native","v",false,added)&&added);expected["new-native"]="v";verify(tx,expected,deadline,true);return true;};
    REQUIRE(database->transact_page(key,std::ref(convert))==db::batch_status::ok);
    auto recreate=[&](page_transaction& tx){bool created;auto fresh=fixture(129);REQUIRE(packed_hash_store::create(tx,key,fresh,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());
        for(const auto& item:fresh){bool removed;REQUIRE(store.erase(item.first,removed)&&removed);}REQUIRE(store.size()==0&&store.expiry()==-1);
        bool added;REQUIRE(store.put("after-empty",std::string(4000,'z'),false,added)&&added&&store.native());packed_hash_store::fields result{{"after-empty",std::string(4000,'z')}};verify(tx,result,-1,true);REQUIRE(store.set_expiry(deadline));bool removed;REQUIRE(store.erase("after-empty",removed)&&removed&&store.size()==0&&store.expiry()==-1);REQUIRE(store.put("after-empty",std::string(4000,'z'),false,added)&&added&&store.expiry()==-1);verify(tx,result,-1,true);return true;};
    REQUIRE(database->transact_page(key,std::ref(recreate))==db::batch_status::ok);
    // A later transaction can prove all old group/native pages were removed.
    auto compact=fixture(64);auto reset=[&](page_transaction& tx){bool created;REQUIRE(packed_hash_store::create(tx,key,compact,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());REQUIRE(store.clear());bool added;REQUIRE(store.put("one","v",false,added)&&added);verify(tx,{{"one","v"}},-1,false);return true;};
    REQUIRE(database->transact_page(key,std::ref(reset))==db::batch_status::ok);
    // Active expiration drops TTL and data before recreation.
    auto expire=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());REQUIRE(store.set_expiry(1));REQUIRE(store.open_live(2)&&store.size()==0&&store.expiry()==-1);bool added;REQUIRE(store.put("new","v",false,added)&&added);return true;};
    REQUIRE(database->transact_page(key,std::ref(expire))==db::batch_status::ok);
    std::string before;REQUIRE(database->get(key,before));
    auto rollback=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());bool added;REQUIRE(store.put("new","changed",false,added));return false;};
    REQUIRE(database->transact_page(key,std::ref(rollback))==db::batch_status::aborted);std::string after;REQUIRE(database->get(key,after)&&before==after);
    auto check=[&](page_transaction& tx){verify(tx,{{"new","v"}},-1,false);return true;};REQUIRE(database->transact_page(key,std::ref(check))==db::batch_status::ok);
    // Ignore a failed read deliberately: the poisoned transaction must not
    // publish the earlier staged deletion of its required group.
    auto missing_page=[&](page_transaction& tx){bool created;REQUIRE(packed_hash_store::create(tx,key,compact,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());const auto group=packed_hash::bucket(compact.begin()->first,packed_hash::creation_bits(compact.size()));REQUIRE(tx.erase(key,group+1));bool found;std::string value;REQUIRE(!store.get(compact.begin()->first,value,found));return true;};
    REQUIRE(database->transact_page(key,std::ref(missing_page))==db::batch_status::aborted);
    REQUIRE(database->transact_page(key,std::ref(check))==db::batch_status::ok);
    // Ordinary-new failures throughout collection/rebuild/commit must leave
    // both root and pages at the original model, not just preserve root bytes.
    unsigned failures=0,successes=0;std::vector<long> points;for(long i=0;i<48;++i)points.push_back(i);for(long i:{64,96,128,192,256,384,512,768,1024})points.push_back(i);
    for(long point:points){auto restore=[&](page_transaction& tx){bool created;return packed_hash_store::create(tx,key,compact,deadline,created)&&created;};REQUIRE(database->transact_page(key,std::ref(restore))==db::batch_status::ok);
        const std::string large(4096,'q');auto operation=[&](page_transaction& tx){packed_hash_store store(tx,key);if(!store.open())return false;bool added;return store.put("oom-large",large,false,added)&&added;};
        fail_allocation=point;const auto result=database->transact_page(key,std::ref(operation));fail_allocation=-1;
        auto model=compact;if(result==db::batch_status::ok){++successes;model["oom-large"]=large;}else ++failures;
        auto validate=[&](page_transaction& tx){verify(tx,model,deadline,result==db::batch_status::ok);return true;};REQUIRE(database->transact_page(key,std::ref(validate))==db::batch_status::ok);
    }
    REQUIRE(failures&&successes);
    // An insertion beyond the root capacity may prepare a leaf edit before
    // native conversion. Aborting must retain every original packed page.
    const auto full = fixture(512);
    auto seed_full = [&](page_transaction& tx) {
        bool created;
        return packed_hash_store::create(tx,key,full,deadline,created) && created;
    };
    REQUIRE(database->transact_page(key,std::ref(seed_full)) == db::batch_status::ok);
    auto abort_overflow = [&](page_transaction& tx) {
        packed_hash::root metadata;
        REQUIRE(packed_hash::decode_root(tx.roots().find(key)->value,metadata) == map_page_status::ok);
        const auto group = packed_hash::bucket("513",metadata.bits);
        REQUIRE(metadata.sizes[group] && metadata.sizes[group] < packed_hash::max_group_fields);
        packed_hash_store store(tx,key); REQUIRE(store.open());
        bool added;
        auto staged = full;
        REQUIRE(store.put(full.begin()->first,"staged",false,added) && !added);
        staged.begin()->second = "staged";
        REQUIRE(store.put("513","value",false,added) && added && store.native());
        staged["513"] = "value";
        verify(tx,staged,deadline,true);
        return false;
    };
    REQUIRE(database->transact_page(key,std::ref(abort_overflow)) == db::batch_status::aborted);
    auto verify_full = [&](page_transaction& tx) { verify(tx,full,deadline,false); return true; };
    REQUIRE(database->transact_page(key,std::ref(verify_full)) == db::batch_status::ok);
    // Overflow due to 513 fields uses the native path with exact added count.
    auto overflow=[&](page_transaction& tx){bool created;auto values=fixture(512);REQUIRE(packed_hash_store::create(tx,key,values,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());bool added;REQUIRE(store.put("513","value",false,added)&&added&&store.native());values["513"]="value";verify(tx,values,deadline,true);return true;};
    REQUIRE(database->transact_page(key,std::ref(overflow))==db::batch_status::ok);
    auto binary=[&](page_transaction& tx){auto model=fixture(64);model.erase(model.begin());model.erase(model.begin());model[""]=std::string("a\0b",3);model[std::string("f\0x",3)]="";bool created;REQUIRE(packed_hash_store::create(tx,key,model,deadline,created)&&created);verify(tx,model,deadline,false);packed_hash_store store(tx,key);REQUIRE(store.open());std::string value;bool found;REQUIRE(store.get("",value,found)&&found&&value==std::string("a\0b",3));REQUIRE(store.take(std::string("f\0x",3),value,found)&&found&&value.empty());return true;};
    REQUIRE(database->transact_page(key,std::ref(binary))==db::batch_status::ok);
}
class observed_mdb:public mdb{public:explicit observed_mdb(size_t n):mdb(n){}using mdb::dbvisit_pages;};
static void inventory(){auto backend=std::make_shared<observed_mdb>(16);shared_db database=backend;REQUIRE(database->open(""));auto seed=[&](page_transaction& tx){bool created;auto model=fixture(129);REQUIRE(packed_hash_store::create(tx,key,model,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());REQUIRE(store.clear());bool added;return store.put("one","v",false,added)&&added;};REQUIRE(database->transact_page(key,std::ref(seed))==db::batch_status::ok);unsigned pages=0;auto visit=[&](uint64_t,const std::string&){++pages;return true;};REQUIRE(backend->dbvisit_pages(key,std::cref(visit))==db::batch_status::ok&&pages==1);auto erase=[&](page_transaction& tx){packed_hash_store store(tx,key);if(!store.open())return false;bool removed;return store.erase("one",removed)&&removed;};REQUIRE(database->transact_page(key,std::ref(erase))==db::batch_status::ok);pages=0;REQUIRE(backend->dbvisit_pages(key,std::cref(visit))==db::batch_status::ok&&pages==0);}
static void native_last_inventory(){auto backend=std::make_shared<observed_mdb>(16);shared_db database=backend;REQUIRE(database->open(""));auto model=fixture(64);std::string survivor;for(const auto& item:model)if(packed_hash::bucket(item.first,4)>=8){survivor=item.first;break;}REQUIRE(!survivor.empty());auto seed=[&](page_transaction& tx){bool created;REQUIRE(packed_hash_store::create(tx,key,model,deadline,created)&&created);packed_hash_store store(tx,key);REQUIRE(store.open());for(const auto& item:model)if(item.first!=survivor){bool removed;REQUIRE(store.erase(item.first,removed)&&removed);}return true;};REQUIRE(database->transact_page(key,std::ref(seed))==db::batch_status::ok);auto operation=[&](page_transaction& tx){packed_hash_store store(tx,key);REQUIRE(store.open());bool added,removed;REQUIRE(store.put(survivor,std::string(4096,'x'),false,added)&&!added&&store.native());REQUIRE(store.erase(survivor,removed)&&removed&&store.size()==0&&store.expiry()==-1);REQUIRE(store.put("after","v",false,added)&&added);verify(tx,{{"after","v"}},-1,true);return true;};REQUIRE(database->transact_page(key,std::ref(operation))==db::batch_status::ok);unsigned pages=0;auto visitor=[&](uint64_t,const std::string&){++pages;return true;};REQUIRE(backend->dbvisit_pages(key,std::cref(visitor))==db::batch_status::ok&&pages==1);}
template<class Backend>class yielding_backend:public Backend{
public:explicit yielding_backend(size_t n):Backend(n){}unsigned roots=0,pages=0;
protected:db::lookup_status dblookup(const std::string& k,std::string& v)override{++roots;acl::fiber::yield();return Backend::dblookup(k,v);}
 bool dbbind_page_reader(const std::string&,db::page_reader&)override{return false;}
 db::lookup_status dbinspect_page(const std::string& k,uint64_t id,std::string& scratch,db::page_inspection& view)override{++pages;acl::fiber::yield();auto result=Backend::dbinspect_page(k,id,scratch,view);acl::fiber::yield();return result;}
};
template<class Backend>static void suspended(){auto backend=std::make_shared<yielding_backend<Backend>>(16);shared_db database=backend;REQUIRE(database->open(""));unsigned complete=0;for(unsigned worker=0;worker<8;++worker){const std::string owner="fiber:"+std::to_string(worker);go [&,owner]{auto model=fixture(64);auto seed=[&](page_transaction& tx){bool created;return packed_hash_store::create(tx,owner,model,deadline,created)&&created;};REQUIRE(database->transact_page(owner,std::ref(seed))==db::batch_status::ok);for(unsigned round=0;round<8;++round){auto operation=[&](page_transaction& tx){packed_hash_store store(tx,owner);REQUIRE(store.open());const auto field=model.begin()->first;const auto value=std::to_string(round);bool added;REQUIRE(store.put(field,value,false,added)&&!added);model[field]=value;std::string actual;bool found;REQUIRE(store.get(field,actual,found)&&found&&actual==value);packed_hash_store::field_list all;REQUIRE(store.all(all)&&(packed_hash_store::fields(all.begin(),all.end())==model));return true;};REQUIRE(database->transact_page(owner,std::ref(operation))==db::batch_status::ok);}++complete;};}acl::fiber::schedule();REQUIRE(complete==8&&backend->roots&&backend->pages);}
static void check_validation_cache() {
    const auto bytes = compact_hash::encode({{"field", "value"}});
    packed_hash::root root;
    const auto group = packed_hash::bucket("field", root.bits);
    const auto other = (group + 1) % root.groups();
    root.sizes[group] = root.sizes[other] = 1;
    packed_hash::page_validation_cache cache;
    const packed_hash::page_view validated(bytes, root, group);
    cache.remember(bytes, 0, validated.context());
    const packed_hash::page_view reused(bytes, root, group, &cache);
    REQUIRE(reused.count() == 1);
    const auto rejected = [&](const std::string& page, unsigned bucket) {
        try { packed_hash::page_view view(page, root, bucket, &cache); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    REQUIRE(rejected(bytes, other));
    auto corrupt = bytes;
    corrupt[5] = 1;
    REQUIRE(rejected(corrupt, group));
}

int main(){acl::acl_cpp_init();go []{};acl::fiber::schedule();check_validation_cache();run<mdb>();inventory();native_last_inventory();suspended<mdb>();
#ifdef HAS_TBB
run<mdb_tbb>();suspended<mdb_tbb>();
#endif
std::puts("packed Hash store checks passed");}
