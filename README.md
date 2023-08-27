# pkv
Parallel KV service supports redis protocol.

# Features
- **Multiple store engines:** Supports rocksdb, wiredtiger, Intel tbb hashmap, stl hashmap, acl hash table;
- **Multiple threads:** Use multiple CPU cores with multiple threads;
- **Coroutine IO:** Use the coroutine lib from acl;
- **Streamed parser:** Streamed parser which is IO independent; 
- **Serialization:** Use yyjson to serialize and deserialize the stored data.

# Support redis commands
- **TYPE**
- **DEL**
- **EXPIRE**
- **TTL**
- **SET**
- **GET**
- **HSET**
- **HGET**
- **HDEL**
- **HGETALL**

# Dependence
- **c++11**
- **CentOS7.6 or Ubuntu 18**
- **Acl project:** https://github.com/acl-dev/acl
- **Yyjson:** https://github.com/ibireme/yyjson
- **Rocksdb:** https://github.com/facebook/rocksdb
- **Wiredtiger:** https://github.com/wiredtiger/wiredtiger
- **Intel oneTBB:** https://github.com/oneapi-src/oneTBB

# Build
## Build and install acl
In the acl source path run below as root:
```shell
 # make & make packinstall
```
Then the libs including libacl_all.a, libfiber_cpp.a and libfiber.a will be installed to `/usr/lib`, and the header files of acl will be installed in `/usr/include/acl-lib`.

## Build and install yyjson, rocksdb, wiredtiger, oneTBB
In the source path of every project run:
```shell
# mkdir build
# cd build
# cmake ..
# make -j 4
# make install
```

## Build pkv
Build pkv with c++17 in default mode:
```shell
# make
```
Or build pkv with c++11
```shell
# make BUILD_WITH_C11=YES
```

The pkv binary will be created.

# Test
Start pkv in the below way, pkv will listen on 19001 as default.

- Run pkv with data being stored memory by oneTBB:
```shell
# ./pkv alone mdb_tbb.cf
```

- Run pkv with data being stored memory by hash table of acl:
```shell
# ./pkv alone mdb_htable.cf
```

- Run pkv with data being stored in rocksdb:
```shell
# ./pkv alone rdb.cf
```

- Run pkv with data being stoared in wiredtiger:
```shell
# ./pkv alone wdb.cf
```

- Run the redis-benchmark in redis to test the pkv's performance
```shell
# ./redis-benchmark -p 19001 -n 10000000 -P 1000 -t set -r 100000000
```