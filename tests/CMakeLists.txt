cmake_minimum_required(VERSION 3.10)
project(pkv_protocol_tests LANGUAGES CXX)
enable_testing()
option(PKV_TEST_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
find_path(ACL_INCLUDE_DIR acl-lib/acl_cpp/lib_acl.hpp)
find_library(ACL_LIBRARY acl_all)
if(NOT ACL_INCLUDE_DIR OR NOT ACL_LIBRARY)
    message(FATAL_ERROR "ACL headers and libacl_all are required; set CMAKE_PREFIX_PATH to their installation prefix")
endif()
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
set(PKV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
foreach(standard 11 17)
    foreach(cache array vector)
        set(target resp_${standard}_${cache})
        add_executable(${target} resp_test.cpp
            ${PKV_ROOT}/coder/redis_object.cpp
            ${PKV_ROOT}/coder/redis_response.cpp
            ${PKV_ROOT}/coder/redis_coder.cpp
            ${PKV_ROOT}/coder/redis_ocache.cpp)
        set_target_properties(${target} PROPERTIES CXX_STANDARD ${standard} CXX_STANDARD_REQUIRED YES)
        target_include_directories(${target} PRIVATE ${PKV_ROOT} ${ACL_INCLUDE_DIR})
        target_link_libraries(${target} PRIVATE ${ACL_LIBRARY} Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
        if(APPLE)
            target_link_libraries(${target} PRIVATE iconv)
        endif()
        if(cache STREQUAL "vector")
            target_compile_definitions(${target} PRIVATE USE_VECTOR)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations)
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                target_compile_options(${target} PRIVATE -Wno-invalid-source-encoding)
            endif()
            if(PKV_TEST_SANITIZERS)
                target_compile_options(${target} PRIVATE -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
                target_link_libraries(${target} PRIVATE -fsanitize=address,undefined)
            endif()
        endif()
        set(alloc_target resp_alloc_${standard}_${cache})
        add_executable(${alloc_target} resp_allocation_test.cpp
            ${PKV_ROOT}/coder/redis_object.cpp
            ${PKV_ROOT}/coder/redis_response.cpp
            ${PKV_ROOT}/coder/redis_coder.cpp
            ${PKV_ROOT}/coder/redis_ocache.cpp)
        set_target_properties(${alloc_target} PROPERTIES
            CXX_STANDARD ${standard} CXX_STANDARD_REQUIRED YES)
        target_include_directories(${alloc_target} PRIVATE
            ${PKV_ROOT} ${ACL_INCLUDE_DIR})
        target_link_libraries(${alloc_target} PRIVATE ${ACL_LIBRARY}
            Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
        if(APPLE)
            target_link_libraries(${alloc_target} PRIVATE iconv)
        endif()
        if(cache STREQUAL "vector")
            target_compile_definitions(${alloc_target} PRIVATE USE_VECTOR)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${alloc_target} PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        if(PKV_TEST_SANITIZERS)
            target_compile_options(${alloc_target} PRIVATE
                -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_libraries(${alloc_target} PRIVATE
                -fsanitize=address,undefined)
        endif()
        add_test(NAME ${alloc_target} COMMAND ${alloc_target})
        set_tests_properties(${alloc_target} PROPERTIES TIMEOUT 45
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endforeach()
endforeach()

# Available when this directory is included by the full server build.
if(TARGET pkv_core)
    foreach(fixture execution_gate transaction_queue watch page_sequence
            page_workspace page_pair native_commit_buffers durability
            move_journal move_domain move_storage move_memory replica_group
            replica_retention family_plan family_workspace database_shutdown
            legacy_expiry
            legacy_transfer command_catalog
            lcs map_bucket page_inspect compact_list compact_erase
            keyspace key_count key_sampling key_crc32 dense_index active_expiry
            set_compare string_digest msetex
            client_state client_registry client_lifecycle client_kill_core
            client_snapshot client_activity client_input list_values_workspace
            hgetdel password_policy response_writer acl_policy acl_file
            acl_audit configuration server_statistics expiry_snapshot
            hyperloglog hyperloglog_fault inline_string geo geo_search
            geo_radius_keys key_wait key_wait_heap key_wait_index
            list_ready zset_ready blocking_io connection_output pubsub
            pubsub_handler stream_store)
        add_executable(${fixture}_test ${fixture}_test.cpp)
        target_link_libraries(${fixture}_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${fixture}_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME ${fixture} COMMAND ${fixture}_test)
        set_tests_properties(${fixture} PROPERTIES TIMEOUT 45
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endforeach()
    find_program(PKV_SEQUENCE_PYTHON NAMES python3)
    if(PKV_SEQUENCE_PYTHON)
        set(family_variant mdb)
        set(compact_erase_backend mdb)
        if(HAS_TBB)
            set(family_variant tbb)
            set(compact_erase_backend mdb_tbb)
        endif()
        add_test(NAME compact_erase_capacity
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/compact_erase_capacity_test.py
                --pkv $<TARGET_FILE:pkv> --backend ${compact_erase_backend})
        set_tests_properties(compact_erase_capacity PROPERTIES TIMEOUT 240
            RUN_SERIAL TRUE ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_test(NAME move_memory_crash
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/move_memory_crash_test.py
                $<TARGET_FILE:move_memory_test> ${family_variant})
        add_test(NAME family_plan_crash
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/family_plan_crash_test.py
                $<TARGET_FILE:family_plan_test> ${family_variant})
        add_test(NAME family_workspace_crash
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/family_plan_crash_test.py
                $<TARGET_FILE:family_workspace_test> ${family_variant})
        set_tests_properties(family_workspace_crash PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        set_tests_properties(family_plan_crash PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        set_tests_properties(move_memory_crash PROPERTIES TIMEOUT 180
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endif()
    if(PKV_SEQUENCE_PYTHON)
        add_executable(move_journal_crash_test move_journal_crash_test.cpp)
        target_link_libraries(move_journal_crash_test PRIVATE pkv_core)
        add_test(NAME move_journal_crash
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/move_journal_crash_test.py
                $<TARGET_FILE:move_journal_crash_test>)
        set_tests_properties(move_journal_crash PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endif()
    if(PKV_SEQUENCE_PYTHON AND (HAS_ROCKSDB OR HAS_WT))
        add_executable(move_recovery_disk_test move_recovery_disk_test.cpp)
        target_link_libraries(move_recovery_disk_test PRIVATE pkv_core)
        add_executable(move_storage_disk_test move_storage_disk_test.cpp)
        target_link_libraries(move_storage_disk_test PRIVATE pkv_core)
        if(HAS_ROCKSDB)
            add_test(NAME move_storage_rdb
                COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_storage_disk_test.py
                    $<TARGET_FILE:move_storage_disk_test> rdb)
            set_tests_properties(move_storage_rdb PROPERTIES
                TIMEOUT 420 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME move_recovery_rdb
                COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_recovery_disk_test.py
                    $<TARGET_FILE:move_recovery_disk_test> rdb)
            set_tests_properties(move_recovery_rdb PROPERTIES TIMEOUT 300
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        endif()
        if(HAS_WT)
            add_test(NAME move_storage_wdb
                COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_storage_disk_test.py
                    $<TARGET_FILE:move_storage_disk_test> wdb)
            set_tests_properties(move_storage_wdb PROPERTIES
                TIMEOUT 420 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME move_recovery_wdb
                COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_recovery_disk_test.py
                    $<TARGET_FILE:move_recovery_disk_test> wdb)
            set_tests_properties(move_recovery_wdb PROPERTIES TIMEOUT 300
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_executable(wt_read_errors_test wt_read_errors_test.cpp)
            target_link_libraries(wt_read_errors_test PRIVATE pkv_core)
            add_test(NAME wt_read_errors COMMAND wt_read_errors_test)
            set_tests_properties(wt_read_errors PROPERTIES TIMEOUT 45
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        endif()
    endif()
    if(PKV_SEQUENCE_PYTHON)
        add_test(NAME move_startup_mdb
            COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/move_startup_test.py
                $<TARGET_FILE:pkv> - mdb)
        set_tests_properties(move_startup_mdb PROPERTIES TIMEOUT 120)
        if(HAS_TBB)
            add_test(NAME move_startup_tbb
                COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_startup_test.py
                    $<TARGET_FILE:pkv> - mdb_tbb)
            set_tests_properties(move_startup_tbb PROPERTIES TIMEOUT 120)
        endif()
        if(HAS_ROCKSDB OR HAS_WT)
            add_executable(move_startup_seed move_startup_seed.cpp)
            target_link_libraries(move_startup_seed PRIVATE pkv_core)
            if(HAS_ROCKSDB)
                add_test(NAME move_startup_rdb
                    COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/move_startup_test.py
                        $<TARGET_FILE:pkv> $<TARGET_FILE:move_startup_seed> rdb)
                set_tests_properties(move_startup_rdb PROPERTIES TIMEOUT 300)
            endif()
            if(HAS_WT)
                add_test(NAME move_startup_wdb
                    COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/move_startup_test.py
                        $<TARGET_FILE:pkv> $<TARGET_FILE:move_startup_seed> wdb)
                set_tests_properties(move_startup_wdb PROPERTIES TIMEOUT 300)
            endif()
        endif()
    endif()
    set(sequence_backends "")
    if(HAS_TBB)
        list(APPEND sequence_backends tbb)
    endif()
    if(HAS_ROCKSDB)
        list(APPEND sequence_backends rdb)
    endif()
    if(HAS_WT)
        list(APPEND sequence_backends wdb)
    endif()
    if(HAS_ROCKSDB OR HAS_WT)
        add_executable(durability_disk_test durability_disk_test.cpp)
        target_link_libraries(durability_disk_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(durability_disk_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
    endif()
    if(PKV_SEQUENCE_PYTHON)
        foreach(sequence_backend ${sequence_backends})
            if(NOT sequence_backend STREQUAL "tbb")
                add_test(NAME durability_${sequence_backend}
                    COMMAND ${PKV_SEQUENCE_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/durability_disk_test.py
                    $<TARGET_FILE:durability_disk_test> ${sequence_backend})
                set_tests_properties(durability_${sequence_backend}
                    PROPERTIES TIMEOUT 90
                    ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
                if(UNIX)
                    add_test(NAME durability_crash_${sequence_backend}
                        COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/durability_disk_test.py
                        $<TARGET_FILE:durability_disk_test>
                        ${sequence_backend} --crash)
                    set_tests_properties(durability_crash_${sequence_backend}
                        PROPERTIES TIMEOUT 120
                        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
                endif()
            endif()
            add_test(NAME zset_ready_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/key_wait_test.py
                $<TARGET_FILE:zset_ready_test> ${sequence_backend})
            set_tests_properties(zset_ready_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME list_ready_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/key_wait_test.py
                $<TARGET_FILE:list_ready_test> ${sequence_backend})
            set_tests_properties(list_ready_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME key_wait_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/key_wait_test.py
                $<TARGET_FILE:key_wait_test> ${sequence_backend})
            set_tests_properties(key_wait_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME active_expiry_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/active_expiry_test.py
                $<TARGET_FILE:active_expiry_test> ${sequence_backend})
            set_tests_properties(active_expiry_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME key_count_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/key_count_test.py
                $<TARGET_FILE:key_count_test> ${sequence_backend})
            set_tests_properties(key_count_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME key_sampling_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/key_sampling_backend_test.py
                $<TARGET_FILE:key_sampling_test> ${sequence_backend})
            set_tests_properties(key_sampling_${sequence_backend}
                PROPERTIES TIMEOUT 120
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME msetex_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/msetex_backend_test.py
                $<TARGET_FILE:msetex_test> ${sequence_backend})
            set_tests_properties(msetex_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME hgetdel_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/hgetdel_backend_test.py
                $<TARGET_FILE:hgetdel_test> ${sequence_backend})
            set_tests_properties(hgetdel_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME page_sequence_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/page_sequence_test.py
                $<TARGET_FILE:page_sequence_test> ${sequence_backend})
            set_tests_properties(page_sequence_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME page_workspace_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/page_workspace_test.py
                $<TARGET_FILE:page_workspace_test> ${sequence_backend})
            set_tests_properties(page_workspace_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME legacy_expiry_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/legacy_expiry_test.py
                $<TARGET_FILE:legacy_expiry_test> ${sequence_backend})
            set_tests_properties(legacy_expiry_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
            add_test(NAME legacy_transfer_${sequence_backend}
                COMMAND ${PKV_SEQUENCE_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/legacy_transfer_test.py
                $<TARGET_FILE:legacy_transfer_test> ${sequence_backend})
            set_tests_properties(legacy_transfer_${sequence_backend}
                PROPERTIES TIMEOUT 90
                ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        endforeach()
    endif()
    add_executable(bitfield_test bitfield_test.cpp)
    target_link_libraries(bitfield_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(bitfield_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bitfield_storage COMMAND bitfield_test)
    set_tests_properties(bitfield_storage PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(bitop_test bitop_test.cpp)
    target_link_libraries(bitop_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(bitop_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bitop_storage COMMAND bitop_test)
    set_tests_properties(bitop_storage PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(bitmap_test bitmap_test.cpp)
    target_link_libraries(bitmap_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(bitmap_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bitmap_storage COMMAND bitmap_test)
    set_tests_properties(bitmap_storage PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_string_test paged_string_test.cpp)
    target_link_libraries(paged_string_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_string_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_string_dao COMMAND paged_string_test)
    set_tests_properties(paged_string_dao PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(string_store_test string_store_test.cpp)
    target_link_libraries(string_store_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(string_store_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME native_string_pages COMMAND string_store_test)
    set_tests_properties(native_string_pages PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    if(HAS_ROCKSDB OR HAS_WT)
        find_program(PKV_STRING_PYTHON NAMES python3)
        if(PKV_STRING_PYTHON)
            foreach(backend ROCKSDB WT)
                if(HAS_${backend})
                    if(backend STREQUAL "ROCKSDB")
                        set(string_backend rdb)
                    else()
                        set(string_backend wdb)
                    endif()
                    add_test(NAME ${string_backend}_string_pages
                        COMMAND ${PKV_STRING_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/string_disk_test.py
                        $<TARGET_FILE:string_store_test> ${string_backend})
                    set_tests_properties(${string_backend}_string_pages
                        PROPERTIES TIMEOUT 90)
                    add_test(NAME ${string_backend}_bitmap_reopen
                        COMMAND ${PKV_STRING_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/string_disk_test.py
                        $<TARGET_FILE:bitmap_test> ${string_backend})
                    set_tests_properties(${string_backend}_bitmap_reopen
                        PROPERTIES TIMEOUT 90)
                    add_test(NAME ${string_backend}_bitop_reopen
                        COMMAND ${PKV_STRING_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/string_disk_test.py
                        $<TARGET_FILE:bitop_test> ${string_backend})
                    set_tests_properties(${string_backend}_bitop_reopen
                        PROPERTIES TIMEOUT 90)
                    add_test(NAME ${string_backend}_bitfield_reopen
                        COMMAND ${PKV_STRING_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/string_disk_test.py
                        $<TARGET_FILE:bitfield_test> ${string_backend})
                    set_tests_properties(${string_backend}_bitfield_reopen
                        PROPERTIES TIMEOUT 90)
                endif()
            endforeach()
        endif()
    endif()
    add_executable(float_increment_test float_increment_test.cpp)
    target_link_libraries(float_increment_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(float_increment_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME float_increment_storage COMMAND float_increment_test)
    set_tests_properties(float_increment_storage PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    if(HAS_TBB)
        add_executable(tbb_storage_test tbb_storage_test.cpp)
        target_link_libraries(tbb_storage_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(tbb_storage_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME tbb_native_storage COMMAND tbb_storage_test)
        set_tests_properties(tbb_native_storage PROPERTIES TIMEOUT 60
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endif()
    if(HAS_WT)
        add_executable(wdb_storage_test wdb_storage_test.cpp)
        target_link_libraries(wdb_storage_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(wdb_storage_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME wiredtiger_native_storage COMMAND wdb_storage_test)
        set_tests_properties(wiredtiger_native_storage PROPERTIES TIMEOUT 90
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        find_program(PKV_WT_PYTHON NAMES python3)
        if(PKV_WT_PYTHON AND UNIX)
            add_test(NAME wiredtiger_crash_recovery
                COMMAND ${PKV_WT_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/wiredtiger_crash_test.py
                    $<TARGET_FILE:wdb_storage_test>)
            set_tests_properties(wiredtiger_crash_recovery PROPERTIES TIMEOUT 90)
        endif()
    endif()
    if(HAS_ROCKSDB)
        add_executable(rdb_storage_test rdb_storage_test.cpp)
        target_link_libraries(rdb_storage_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(rdb_storage_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME rocksdb_native_storage COMMAND rdb_storage_test)
        set_tests_properties(rocksdb_native_storage PROPERTIES TIMEOUT 90
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        find_program(PKV_ROCKS_PYTHON NAMES python3)
        if(PKV_ROCKS_PYTHON AND UNIX)
            add_test(NAME rocksdb_crash_recovery
                COMMAND ${PKV_ROCKS_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb_crash_test.py
                    $<TARGET_FILE:rdb_storage_test>)
            set_tests_properties(rocksdb_crash_recovery PROPERTIES TIMEOUT 90)
        endif()
    endif()
    add_executable(batch_failure_test batch_failure_test.cpp)
    target_link_libraries(batch_failure_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(batch_failure_test PRIVATE -Wno-invalid-source-encoding)
    endif()
    add_test(NAME batch_allocation_failure COMMAND batch_failure_test)
    set_tests_properties(batch_allocation_failure PROPERTIES TIMEOUT 30 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_zset_test paged_zset_test.cpp)
    target_link_libraries(paged_zset_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_zset_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_zset_dao COMMAND paged_zset_test)
    set_tests_properties(paged_zset_dao PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(zset_range_test zset_range_test.cpp)
    target_link_libraries(zset_range_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(zset_range_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME native_zset_ranges COMMAND zset_range_test)
    set_tests_properties(native_zset_ranges PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(zset_store_test zset_store_test.cpp)
    target_link_libraries(zset_store_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(zset_store_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bounded_zset_store COMMAND zset_store_test)
    set_tests_properties(bounded_zset_store PROPERTIES TIMEOUT 90
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_set_test paged_set_test.cpp)
    target_link_libraries(paged_set_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_set_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_set_dao COMMAND paged_set_test)
    set_tests_properties(paged_set_dao PROPERTIES TIMEOUT 60
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_hash_test paged_hash_test.cpp)
    target_link_libraries(paged_hash_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_hash_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_hash_dao COMMAND paged_hash_test)
    set_tests_properties(paged_hash_dao PROPERTIES TIMEOUT 60
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(map_iteration_test map_iteration_test.cpp)
    target_link_libraries(map_iteration_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(map_iteration_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bounded_map_iteration COMMAND map_iteration_test)
    set_tests_properties(bounded_map_iteration PROPERTIES TIMEOUT 60
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(map_store_test map_store_test.cpp)
    target_link_libraries(map_store_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(map_store_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME bounded_map_store COMMAND map_store_test)
    set_tests_properties(bounded_map_store PROPERTIES TIMEOUT 60
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_list_test paged_list_test.cpp)
    target_link_libraries(paged_list_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_list_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_list_dao COMMAND paged_list_test)
    set_tests_properties(paged_list_dao PROPERTIES TIMEOUT 45
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(paged_key_test paged_key_test.cpp)
    target_link_libraries(paged_key_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(paged_key_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME paged_key_metadata COMMAND paged_key_test)
    set_tests_properties(paged_key_metadata PROPERTIES TIMEOUT 30
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(list_edit_test list_edit_test.cpp)
    target_link_libraries(list_edit_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(list_edit_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME list_edits COMMAND list_edit_test)
    set_tests_properties(list_edits PROPERTIES TIMEOUT 45
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(list_store_test list_store_test.cpp)
    target_link_libraries(list_store_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(list_store_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME list_store COMMAND list_store_test)
    set_tests_properties(list_store PROPERTIES TIMEOUT 45
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(page_storage_test page_storage_test.cpp)
    target_link_libraries(page_storage_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(page_storage_test PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_test(NAME page_storage COMMAND page_storage_test)
    set_tests_properties(page_storage PROPERTIES TIMEOUT 30
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(list_page_test list_page_test.cpp)
    target_link_libraries(list_page_test PRIVATE pkv_core)
    add_test(NAME list_page_codec COMMAND list_page_test)
    set_tests_properties(list_page_codec PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(codec_test codec_test.cpp)
    target_link_libraries(codec_test PRIVATE pkv_core)
    add_test(NAME record_codec COMMAND codec_test)
    set_tests_properties(record_codec PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    add_executable(storage_test storage_test.cpp)
    target_link_libraries(storage_test PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(storage_test PRIVATE -Wno-invalid-source-encoding)
    endif()
    add_test(NAME storage COMMAND storage_test)
    set_tests_properties(storage PROPERTIES TIMEOUT 45 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    if(UNIX)
        add_executable(source_exec_test source_exec_test.cpp)
        target_link_libraries(source_exec_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(source_exec_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME source_exec COMMAND source_exec_test)
        set_tests_properties(source_exec PROPERTIES TIMEOUT 60
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        if(PKV_SEQUENCE_PYTHON)
            foreach(source_backend ${sequence_backends})
                if(NOT source_backend STREQUAL "tbb")
                    add_test(NAME source_exec_${source_backend}
                        COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/source_exec_test.py
                        $<TARGET_FILE:source_exec_test> ${source_backend})
                    set_tests_properties(source_exec_${source_backend}
                        PROPERTIES TIMEOUT 90
                        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
                endif()
            endforeach()
        endif()
        add_executable(exec_replication_test exec_replication_test.cpp)
        target_link_libraries(exec_replication_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(exec_replication_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME exec_replication COMMAND exec_replication_test)
        set_tests_properties(exec_replication PROPERTIES TIMEOUT 60
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_executable(ordered_replication_test ordered_replication_test.cpp)
        target_link_libraries(ordered_replication_test PRIVATE pkv_core)
        add_test(NAME ordered_replication COMMAND ordered_replication_test)
        set_tests_properties(ordered_replication PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_executable(move_publication_test move_publication_test.cpp)
        target_link_libraries(move_publication_test PRIVATE pkv_core)
        add_test(NAME move_publication COMMAND move_publication_test)
        set_tests_properties(move_publication PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_executable(move_key_test move_key_test.cpp)
        target_link_libraries(move_key_test PRIVATE pkv_core)
        add_test(NAME move_key COMMAND move_key_test)
        set_tests_properties(move_key PROPERTIES TIMEOUT 120
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        if(HAS_ROCKSDB OR HAS_WT)
            add_executable(native_paths_test native_paths_test.cpp)
            target_link_libraries(native_paths_test PRIVATE pkv_core)
        endif()
        if(PKV_SEQUENCE_PYTHON)
            foreach(move_backend rdb wdb)
                if((move_backend STREQUAL "rdb" AND HAS_ROCKSDB) OR
                    (move_backend STREQUAL "wdb" AND HAS_WT))
                    add_test(NAME move_online_recovery_${move_backend}
                        COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/move_online_recovery_test.py
                        --pkv $<TARGET_FILE:pkv> --backend ${move_backend}
                        --output ${CMAKE_CURRENT_BINARY_DIR}/move-online.json)
                    set_tests_properties(move_online_recovery_${move_backend}
                        PROPERTIES TIMEOUT 180
                        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
                    add_test(NAME native_paths_${move_backend}
                        COMMAND ${PKV_SEQUENCE_PYTHON}
                        ${CMAKE_CURRENT_SOURCE_DIR}/native_paths_test.py
                        --pkv $<TARGET_FILE:pkv>
                        --helper $<TARGET_FILE:native_paths_test>
                        --backend ${move_backend})
                    set_tests_properties(native_paths_${move_backend}
                        PROPERTIES TIMEOUT 180
                        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
                endif()
            endforeach()
        endif()
        add_executable(live_page_replication_test
            live_page_replication_test.cpp)
        target_link_libraries(live_page_replication_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(live_page_replication_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME live_page_replication COMMAND live_page_replication_test)
        set_tests_properties(live_page_replication PROPERTIES TIMEOUT 30
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_executable(page_replication_test page_replication_test.cpp)
        target_link_libraries(page_replication_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(page_replication_test PRIVATE
                -Wno-invalid-source-encoding)
        endif()
        add_test(NAME page_replication COMMAND page_replication_test)
        set_tests_properties(page_replication PROPERTIES TIMEOUT 30
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
        add_executable(rpc_test rpc_test.cpp)
        target_link_libraries(rpc_test PRIVATE pkv_core)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(rpc_test PRIVATE -Wno-invalid-source-encoding)
        endif()
        add_test(NAME rpc_transport COMMAND rpc_test)
        set_tests_properties(rpc_transport PROPERTIES TIMEOUT 20 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
    endif()
    option(PKV_SOCKET_TESTS "Run loopback socket integration tests" ON)
    set(PKV_REFERENCE_REDIS "" CACHE FILEPATH "Optional redis-server for comparison tests")
    if(PKV_SOCKET_TESTS)
        find_program(PKV_PYTHON_EXECUTABLE NAMES python3)
        if(PKV_PYTHON_EXECUTABLE)
            set(socket_args --pkv $<TARGET_FILE:pkv>)
            if(PKV_REFERENCE_REDIS)
                list(APPEND socket_args --redis ${PKV_REFERENCE_REDIS})
            endif()
            if(PKV_REFERENCE_REDIS)
                add_test(NAME dbsize_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/dbsize_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(dbsize_commands PROPERTIES TIMEOUT 60)
                if(HAS_TBB)
                    set_property(TEST dbsize_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST dbsize_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST dbsize_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME keyspace_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/keyspace_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(keyspace_commands PROPERTIES TIMEOUT 60)
                if(HAS_TBB)
                    set_property(TEST keyspace_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST keyspace_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST keyspace_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME random_key_time_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/random_key_time_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(random_key_time_commands
                    PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST random_key_time_commands APPEND
                        PROPERTY ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST random_key_time_commands APPEND
                        PROPERTY ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST random_key_time_commands APPEND
                        PROPERTY ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME lcs_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/lcs_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(lcs_commands PROPERTIES TIMEOUT 60)
                if(HAS_TBB)
                    set_property(TEST lcs_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST lcs_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST lcs_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME set_compare_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/set_compare_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(set_compare_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST set_compare_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST set_compare_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST set_compare_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME digest_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/digest_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(digest_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST digest_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST digest_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST digest_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME msetex_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/msetex_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(msetex_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST msetex_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST msetex_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST msetex_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME hgetdel_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/hgetdel_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(hgetdel_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST hgetdel_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST hgetdel_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST hgetdel_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME client_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/client_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(client_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST client_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST client_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST client_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME acl_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/acl_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(acl_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST acl_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST acl_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST acl_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME acl_file_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/acl_file_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(acl_file_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST acl_file_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST acl_file_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST acl_file_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME acl_log_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/acl_log_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(acl_log_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST acl_log_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST acl_log_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST acl_log_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME configuration_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/configuration_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(configuration_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST configuration_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST configuration_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST configuration_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME info_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/info_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(info_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME hyperloglog_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/hyperloglog_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(hyperloglog_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST hyperloglog_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST hyperloglog_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST hyperloglog_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME geo_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/geo_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(geo_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST geo_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST geo_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST geo_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME geo_search_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/geo_search_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(geo_search_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST geo_search_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST geo_search_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST geo_search_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME geo_radius_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/geo_radius_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(geo_radius_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST geo_radius_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST geo_radius_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST geo_radius_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME pubsub_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/pubsub_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(pubsub_commands PROPERTIES TIMEOUT 180)
                if(HAS_TBB)
                    set_property(TEST pubsub_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST pubsub_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST pubsub_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME blocking_zset_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/blocking_zset_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(blocking_zset_commands PROPERTIES
                    TIMEOUT 180)
                if(HAS_TBB)
                    set_property(TEST blocking_zset_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST blocking_zset_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST blocking_zset_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME blocking_list_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/blocking_list_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(blocking_list_commands PROPERTIES
                    TIMEOUT 180)
                if(HAS_TBB)
                    set_property(TEST blocking_list_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST blocking_list_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST blocking_list_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME replica_readonly_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/replica_readonly_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(replica_readonly_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST replica_readonly_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST replica_readonly_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST replica_readonly_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME client_info_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/client_info_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(client_info_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST client_info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST client_info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST client_info_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME client_kill_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/client_kill_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(client_kill_commands PROPERTIES
                    TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST client_kill_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST client_kill_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST client_kill_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME resp3_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/resp3_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(resp3_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST resp3_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST resp3_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST resp3_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME auth_commands
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/auth_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(auth_commands PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST auth_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST auth_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST auth_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
                add_test(NAME command_metadata
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/command_info_test.py
                    --pkv $<TARGET_FILE:pkv> --redis ${PKV_REFERENCE_REDIS})
                set_tests_properties(command_metadata PROPERTIES TIMEOUT 60)
                if(HAS_TBB)
                    set_property(TEST command_metadata APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST command_metadata APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST command_metadata APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
            endif()
            if(APPLE)
                add_test(NAME listener_distribution
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/listener_distribution_test.py
                    --pkv $<TARGET_FILE:pkv>)
                set_tests_properties(listener_distribution
                    PROPERTIES TIMEOUT 90)
            endif()
            add_test(NAME server_exit COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/server_exit_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(server_exit PROPERTIES TIMEOUT 120)
            if(HAS_TBB)
                set_property(TEST server_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
            elseif(HAS_ROCKSDB)
                set_property(TEST server_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=rdb)
            elseif(HAS_WT)
                set_property(TEST server_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=wdb)
            endif()
            set(callback_exit_options)
            if(HAS_WT AND NOT HAS_TBB AND NOT HAS_ROCKSDB)
                add_executable(wt_clean_exit_check wt_clean_exit_check.cpp)
                target_include_directories(wt_clean_exit_check PRIVATE
                    ${WIREDTIGER_INCLUDE_DIR})
                target_link_libraries(wt_clean_exit_check PRIVATE
                    ${WIREDTIGER_LIBRARY})
                list(APPEND callback_exit_options --clean-check
                    $<TARGET_FILE:wt_clean_exit_check>)
            endif()
            add_test(NAME server_callback_exit
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/server_callback_exit_test.py
                --pkv $<TARGET_FILE:pkv> ${callback_exit_options})
            set_tests_properties(server_callback_exit PROPERTIES TIMEOUT 240)
            if(HAS_TBB)
                set_property(TEST server_callback_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
            elseif(HAS_ROCKSDB)
                set_property(TEST server_callback_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=rdb)
            elseif(HAS_WT)
                set_property(TEST server_callback_exit APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=wdb)
            endif()
            if(APPLE)
                add_library(fiber_hook_gap SHARED fiber_hook_gap.cpp)
                set(fiber_hook_backend mdb)
                if(HAS_TBB)
                    set(fiber_hook_backend mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set(fiber_hook_backend rdb)
                elseif(HAS_WT)
                    set(fiber_hook_backend wdb)
                endif()
                add_test(NAME fiber_hook_startup
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/fiber_hook_startup_test.py
                    --pkv $<TARGET_FILE:pkv>
                    --hook $<TARGET_FILE:fiber_hook_gap>
                    --backend ${fiber_hook_backend} ${callback_exit_options})
                set_tests_properties(fiber_hook_startup
                    PROPERTIES TIMEOUT 120)
            endif()
            if(UNIX)
                add_executable(startup_signal_helper startup_signal_helper.cpp)
                target_link_libraries(startup_signal_helper PRIVATE pkv_core)
                set(startup_signal_options)
                if(TARGET move_startup_seed)
                    list(APPEND startup_signal_options --seed
                        $<TARGET_FILE:move_startup_seed>)
                endif()
                add_test(NAME startup_signal COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/startup_signal_test.py
                    --pkv $<TARGET_FILE:pkv>
                    --helper $<TARGET_FILE:startup_signal_helper>
                    ${startup_signal_options})
                set_tests_properties(startup_signal PROPERTIES TIMEOUT 420)
                if(HAS_TBB)
                    set_property(TEST startup_signal APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST startup_signal APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST startup_signal APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
            endif()
            add_test(NAME active_expiry_cleanup
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/active_expiry_socket_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(active_expiry_cleanup PROPERTIES TIMEOUT 45)
            if(HAS_TBB)
                set_property(TEST active_expiry_cleanup APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
            elseif(HAS_ROCKSDB)
                set_property(TEST active_expiry_cleanup APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=rdb)
            elseif(HAS_WT)
                set_property(TEST active_expiry_cleanup APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=wdb)
            endif()
            add_test(NAME socket_compatibility COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.py ${socket_args})
            set_tests_properties(socket_compatibility PROPERTIES TIMEOUT 45)
            add_test(NAME hash_sampling COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/hash_sampling_test.py
                ${socket_args})
            set_tests_properties(hash_sampling PROPERTIES TIMEOUT 45)
            add_test(NAME float_increment_commands
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/float_increment_test.py
                ${socket_args})
            set_tests_properties(float_increment_commands PROPERTIES TIMEOUT 90)
            add_test(NAME bitmap_commands COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/bitmap_test.py ${socket_args})
            set_tests_properties(bitmap_commands PROPERTIES TIMEOUT 90)
            add_test(NAME bitop_commands COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/bitop_test.py ${socket_args})
            set_tests_properties(bitop_commands PROPERTIES TIMEOUT 90)
            add_test(NAME transaction_commands
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/transaction_test.py ${socket_args})
            set_tests_properties(transaction_commands PROPERTIES TIMEOUT 120)
            if(PKV_REFERENCE_REDIS)
                add_test(NAME move_commands COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/move_test.py ${socket_args})
                set_tests_properties(move_commands PROPERTIES TIMEOUT 180)
                if(HAS_TBB)
                    set_property(TEST move_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                elseif(HAS_ROCKSDB)
                    set_property(TEST move_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=rdb)
                elseif(HAS_WT)
                    set_property(TEST move_commands APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=wdb)
                endif()
            endif()
            add_test(NAME bitfield_commands COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/bitfield_test.py ${socket_args})
            set_tests_properties(bitfield_commands PROPERTIES TIMEOUT 90)
            add_test(NAME exec_live_stream COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/exec_live_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(exec_live_stream PROPERTIES TIMEOUT 120)
            if(UNIX)
                add_test(NAME ordered_live_stream
                    COMMAND ${PKV_PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/ordered_live_test.py
                    --pkv $<TARGET_FILE:pkv>)
                set_tests_properties(ordered_live_stream PROPERTIES TIMEOUT 120)
                if(HAS_TBB)
                    set_property(TEST ordered_live_stream APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
                else()
                    set_property(TEST ordered_live_stream APPEND PROPERTY
                        ENVIRONMENT PKV_TEST_BACKEND=mdb)
                endif()
            endif()
            add_test(NAME move_live_stream COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/move_live_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(move_live_stream PROPERTIES TIMEOUT 180)
            if(HAS_TBB)
                set_property(TEST move_live_stream APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
            elseif(HAS_ROCKSDB)
                set_property(TEST move_live_stream APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=rdb)
            elseif(HAS_WT)
                set_property(TEST move_live_stream APPEND PROPERTY
                    ENVIRONMENT PKV_TEST_BACKEND=wdb)
            endif()
            add_test(NAME replica_retention_stream
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/replica_retention_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(replica_retention_stream PROPERTIES TIMEOUT 60)
            add_test(NAME replication_stream COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/replication_test.py --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(replication_stream PROPERTIES TIMEOUT 30)
            add_test(NAME page_live_stream
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/page_live_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(page_live_stream PROPERTIES TIMEOUT 45)
            add_test(NAME replica_queue_limits
                COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/replica_limits_test.py
                --pkv $<TARGET_FILE:pkv>)
            set_tests_properties(replica_queue_limits PROPERTIES TIMEOUT 45)
            add_test(NAME cluster_routing COMMAND ${PKV_PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.py ${socket_args} --cluster-only)
            set_tests_properties(cluster_routing PROPERTIES TIMEOUT 45)
            if(HAS_TBB)
                set_property(TEST socket_compatibility hash_sampling
                    float_increment_commands bitmap_commands bitop_commands
                    bitfield_commands transaction_commands
                    replication_stream
                    page_live_stream exec_live_stream replica_queue_limits
                    replica_retention_stream
                    cluster_routing
                    APPEND PROPERTY ENVIRONMENT PKV_TEST_BACKEND=mdb_tbb)
            elseif(HAS_ROCKSDB)
                set_property(TEST socket_compatibility hash_sampling
                    float_increment_commands bitmap_commands bitop_commands
                    bitfield_commands transaction_commands
                    replication_stream
                    page_live_stream exec_live_stream replica_queue_limits
                    replica_retention_stream
                    cluster_routing
                    APPEND PROPERTY ENVIRONMENT PKV_TEST_BACKEND=rdb)
            elseif(HAS_WT)
                # This fixture issues many individually synced disk commits.
                set_tests_properties(socket_compatibility PROPERTIES
                    TIMEOUT 180)
                set_property(TEST socket_compatibility hash_sampling
                    float_increment_commands bitmap_commands bitop_commands
                    bitfield_commands transaction_commands
                    replication_stream
                    page_live_stream exec_live_stream replica_queue_limits
                    replica_retention_stream
                    cluster_routing
                    APPEND PROPERTY ENVIRONMENT PKV_TEST_BACKEND=wdb)
            endif()
        else()
            message(STATUS "Python 3 not found; skipping socket integration tests")
        endif()
    endif()
endif()

option(PKV_BUILD_BENCHMARKS "Build record codec microbenchmarks" OFF)
if(TARGET pkv_core AND PKV_BUILD_BENCHMARKS)
    add_executable(key_sampling_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/benchmark_key_sampling.cpp)
    target_link_libraries(key_sampling_bench PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(key_sampling_bench PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_executable(key_wait_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/benchmark_key_wait.cpp)
    target_link_libraries(key_wait_bench PRIVATE pkv_core)
    add_executable(expiry_path_bench expiry_path_bench.cpp)
    target_link_libraries(expiry_path_bench PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(expiry_path_bench PRIVATE
            -Wno-invalid-source-encoding)
    endif()
    add_executable(client_snapshot_bench client_snapshot_bench.cpp)
    target_link_libraries(client_snapshot_bench PRIVATE pkv_core)
    add_executable(codec_bench codec_bench.cpp)
    target_link_libraries(codec_bench PRIVATE pkv_core)
    add_executable(string_storage_bench string_storage_bench.cpp)
    target_link_libraries(string_storage_bench PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(string_storage_bench PRIVATE
            -Wno-invalid-source-encoding)
    endif()
endif()

if(TARGET pkv_core AND PKV_BUILD_BENCHMARKS AND HAS_TBB)
    add_executable(memory_backend_bench memory_backend_bench.cpp)
    target_link_libraries(memory_backend_bench PRIVATE pkv_core)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(memory_backend_bench PRIVATE
            -Wno-invalid-source-encoding)
    endif()
endif()
