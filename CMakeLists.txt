cmake_minimum_required(VERSION 2.8)
project(pkv)

if (${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	message(FATAL_ERROR "Please into another dir to build!")
endif()

if (${CMAKE_BINARY_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
	message(FATAL_ERROR "Please into another dir to build!")
endif()

if (CMAKE_BUILD_TYPE STREQUAL "RELEASE")
    message(STATUS "build pkv for release version")
elseif (CMAKE_BUILD_TYPE STREQUAL "DEBUG")
	message(STATUS "build pkv for debug version")
else()
    message(STATUS "build pkv for default version")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
	add_definitions("-Wno-invalid-source-encoding")
    include_directories("/usr/local/include")
    SET(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
    SET(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
else()
	message(FATAL_ERROR "unknown CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
endif()

##############################################################################

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(base_path ${CMAKE_CURRENT_SOURCE_DIR})
set(src_paths
    ${base_path}
    ${base_path}/common
    ${base_path}/test
    ${base_path}/coder
    ${base_path}/action
    ${base_path}/dao
    ${base_path}/dao/json
    ${base_path}/db
    ${base_path}/db/rocksdb
    ${base_path}/db/wt
    ${base_path}/db/mdb
)

foreach(iter ${src_paths})
    aux_source_directory(${iter} src_files)
endforeach()

##############################################################################

#SET(CMAKE_VERBOSE_MAKEFILE on)

add_definitions(
    "-g"
    "-W"
    "-O3"
    "-Wall"
    "-Werror"
    "-Wshadow"
    "-Wformat"
    "-Wpointer-arith"
    "-D_REENTRANT"
    "-Wno-long-long"
    "-Wuninitialized"
    "-D_POSIX_PTHREAD_SEMANTICS"
    "-fexceptions"
    "-Wno-unused-parameter"
    "-Wno-error=deprecated-declarations"
    "-Wno-deprecated-declarations"
    "-fPIC"
    "-DHAS_TBB"
)

if (${BUILD_WITH_C11} MATCHES "YES")
    add_definitions("-std=c++11")
else()
    add_definitions("-std=c++17")
endif()

find_library(acl_lib acl_all PATHS /usr/lib /usr/local/lib)
find_library(fiber_cpp_lib fiber_cpp PATHS /usr/lib /usr/local/lib)
find_library(fiber_lib fiber PATHS /usr/lib /usr/local/lib)

find_library(yyjson NAMES yyjson PATHS /usr/lib /usr/local/lib)
set(lib_all ${fiber_cpp_lib} ${acl_lib} ${yyjson})

if(${HAS_ROCKSDB} MATCHES "YES")
    find_library(rocksdb NAMES rocksdb PATHS /usr/lib /usr/local/lib /usr/local/lib64)
#    set(rocksdb /lib64/librocksdb.a)
    add_definitions("-DHAS_ROCKSDB")
    set(lib_all ${lib_all} ${rocksdb})
endif()

if(${HAS_WT} MATCHES "YES")
    find_library(wiredtiger NAMES wiredtiger PATHS /usr/lib /usr/local/lib /usr/local/lib64)
#    set(wiredtiger /usr/local/lib64/libwiredtiger.so)
    add_definitions("-DHAS_WT")
    set(lib_all ${lib_all} ${wiredtiger})
endif()

if(${HAS_JEMALLOC} MATCHES "YES")
    find_library(jemalloc NAMES jemalloc PATHS /usr/lib /usr/local/lib /usr/local/lib64)
#    set(jemalloc /usr/local/lib/libjemalloc.a)
    add_definitions("-DHAS_JEAMALLOC")
    set(lib_all ${lib_all} ${jemalloc})
endif()

#set(uring -luring-ffi)
#set(iconv -liconv)

set(lib_all ${lib_all} ${fiber})

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(lib_all ${lib_all} -liconv)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(lib_all ${lib_all} ${uring} ${iconv} ${jemalloc})
endif()

find_library(tbb_lib tbb PATHS /usr/lib /usr/local/lib)
find_library(tbbmalloc_lib tbbmalloc PATHS /usr/lib /usr/local/lib)
set(tbb_libs ${tbb_lib} ${tbbmalloc_lib})
#set(tbb_libs ${tbb_lib})
#set(tbb_lib /usr/local/lib64/libtbb.a /usr/local/lib64/libtbbmalloc.a)
set(lib_all ${lib_all} ${fiber_lib} ${tbb_libs} -lz -lpthread -ldl)

set(output_path ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${output_path})
link_directories(${output_path})

add_executable(pkv ${src_files})
target_link_libraries(pkv ${lib_all})

###############################################################################
