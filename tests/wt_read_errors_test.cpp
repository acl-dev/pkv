#include "db/wt/wt.h"
#include "db/page_transaction.h"
#include "db/disk_address.h"
#include "coder/string_page.h"
#include <wiredtiger.h>
#include <acl-lib/acl_cpp/lib_acl.hpp>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <map>
#include <unistd.h>

using namespace pkv;
using session_open = decltype(((WT_CONNECTION*)nullptr)->open_session);
using cursor_open = decltype(((WT_SESSION*)nullptr)->open_cursor);
using session_close = decltype(((WT_SESSION*)nullptr)->close);
struct cursor_methods {
    decltype(((WT_CURSOR*)nullptr)->search) search;
    decltype(((WT_CURSOR*)nullptr)->search_near) near;
    decltype(((WT_CURSOR*)nullptr)->next) next;
    decltype(((WT_CURSOR*)nullptr)->get_key) key;
    decltype(((WT_CURSOR*)nullptr)->get_value) value;
    decltype(((WT_CURSOR*)nullptr)->close) close;
    int position = 123;
};
static session_open native_session;
static cursor_open native_cursor;
static session_close native_close;
static std::map<WT_CURSOR*, cursor_methods> methods;
static int failure = WT_NOTFOUND;
static bool key_error = false, positioned = true;
static unsigned reads = 0;
static int fail_read(WT_CURSOR* cursor, ...) {
    auto& method = methods.at(cursor);
    ++reads; positioned = positioned && method.position == 0;
    cursor->get_key = method.key; cursor->get_value = method.value;
    return failure;
}
static cursor_methods& restore(WT_CURSOR* cursor) {
    auto& method = methods.at(cursor);
    cursor->get_key = method.key; cursor->get_value = method.value;
    return method;
}
static int position(WT_CURSOR* cursor, int result) {
    methods.at(cursor).position = result;
    if (!result) {
        if (key_error) cursor->get_key = fail_read;
        else cursor->get_value = fail_read;
    }
    return result;
}
static int search(WT_CURSOR* cursor) {
    return position(cursor, restore(cursor).search(cursor));
}
static int near(WT_CURSOR* cursor, int* comparison) {
    return position(cursor, restore(cursor).near(cursor, comparison));
}
static int next(WT_CURSOR* cursor) {
    return position(cursor, restore(cursor).next(cursor));
}
static int close_cursor(WT_CURSOR* cursor) {
    const auto original = methods.at(cursor);
    cursor->get_key = original.key; cursor->get_value = original.value;
    cursor->search = original.search; cursor->search_near = original.near;
    cursor->next = original.next; cursor->close = original.close;
    methods.erase(cursor); return original.close(cursor);
}
static int close_session(WT_SESSION* session, const char* config) {
    session->open_cursor = native_cursor; session->close = native_close;
    return native_close(session, config);
}
static int open_cursor(WT_SESSION* session, const char* uri, WT_CURSOR* copy,
    const char* config, WT_CURSOR** out) {
    const int result = native_cursor(session, uri, copy, config, out);
    if (!result && !std::strncmp(uri, "table:pkv_", 10)) {
        auto* cursor = *out;
        cursor_methods original;
        original.search = cursor->search; original.near = cursor->search_near;
        original.next = cursor->next; original.key = cursor->get_key;
        original.value = cursor->get_value; original.close = cursor->close;
        methods[cursor] = original; cursor->close = close_cursor;
        cursor->search = search; cursor->search_near = near;
        cursor->next = next;
    }
    return result;
}
static int open_session(WT_CONNECTION* connection, WT_EVENT_HANDLER* handler,
    const char* config, WT_SESSION** out) {
    const int result = native_session(connection, handler, config, out);
    if (!result) {
        methods.clear();
        native_cursor = (*out)->open_cursor; native_close = (*out)->close;
        (*out)->open_cursor = open_cursor; (*out)->close = close_session;
    }
    return result;
}
struct temporary {
    std::string path;
    temporary() {
        char pattern[] = "/tmp/pkv-wt-read-errors-XXXXXX";
        const auto* name = ::mkdtemp(pattern);
        if (!name) std::abort();
        path = name;
    }
    ~temporary() {
        auto* directory = ::opendir(path.c_str());
        if (directory) {
            while (const auto* item = ::readdir(directory)) {
                const std::string name(item->d_name);
                if (name != "." && name != "..")
                    ::unlink((path + "/" + name).c_str());
            }
            ::closedir(directory);
        }
        ::rmdir(path.c_str());
    }
};
static void reset() { reads = 0; positioned = true; }
static bool outcome(const char* name, int actual, int expected) {
    std::cout << name << " read=" << failure << " positioned=" << positioned
        << " calls=" << reads << " outcome=" << actual << '\n';
    return actual == expected && positioned && reads > 0;
}
int main() {
    acl::acl_cpp_init();
    temporary files;
    wt database(0); // No cached sessions can bypass the native cursor hook.
    if (!database.open(files.path.c_str())) return 1;
    string_root value; value.length = 16384; value.pages = 1;
    std::string encoded;
    if (string_page_codec::encode_root(value, encoded) !=
        string_page_status::ok) return 1;
    const std::string payload(16384, 'p');
    std::vector<db::batch_entry> roots;
    roots.emplace_back("present", false, encoded);
    roots.front().action = db::update_action::write;
    if (database.apply(roots, {page_change("present", 1, payload,
        db::update_action::write)}, false) != db::batch_status::ok) return 1;
    auto* connection = database.get_db();
    native_session = connection->open_session;
    connection->open_session = open_session;
    bool ok = true;
    for (int code : {WT_NOTFOUND, EIO, WT_ROLLBACK}) {
        failure = code; key_error = false;
        for (unsigned kind = 0; kind < 3; ++kind) {
            reset(); std::string out("unchanged");
            const auto status = kind == 0 ?
                database.bounded_root("present", out) :
                kind == 1 ? database.lookup("present", out)
                : database.page("present", 1, out);
            const bool valid = outcome(kind == 0 ? "bounded" :
                kind == 1 ? "root" : "page", static_cast<int>(status),
                static_cast<int>(db::lookup_status::error));
            ok = valid && out == "unchanged" && ok;
        }
        for (bool keys : {false, true}) {
            key_error = keys; reset(); unsigned seen = 0;
            const auto status = database.visit_pages("present",
                [&](uint64_t, const std::string&) { ++seen; return true; });
            const bool valid = outcome(keys ? "page-key" : "page-value",
                static_cast<int>(status),
                static_cast<int>(db::batch_status::error));
            ok = valid && !seen && ok;
        }
        key_error = true; reset();
        std::vector<std::string> keys;
        const auto scanned = database.scan(disk_address::bucket("present"),
            keys);
        const bool scan_ok = outcome("scan", static_cast<int>(scanned),
            static_cast<int>(db::scan_status::error));
        ok = scan_ok && keys.empty() && ok;
        reset(); unsigned seen = 0;
        const auto visited = database.visit_keys([&](const std::string&) {
            ++seen; return true;
        });
        const bool visit_ok = outcome("keys", static_cast<int>(visited),
            static_cast<int>(db::scan_status::error));
        ok = visit_ok && !seen && ok;
    }
    key_error = false;
    for (unsigned kind = 0; kind < 3; ++kind) {
        reset(); std::string out("unchanged");
        const auto status = kind == 0 ? database.bounded_root("missing", out)
            : kind == 1 ? database.lookup("missing", out)
            : database.page("missing", 1, out);
        ok = status == db::lookup_status::missing && out == "unchanged" &&
            !reads && ok;
    }
    connection->open_session = native_session;
    std::string actual;
    ok = database.bounded_root("present", actual) == db::lookup_status::found
        && actual == encoded && ok;
    ok = database.lookup("present", actual) == db::lookup_status::found &&
        actual == encoded && ok;
    ok = database.page("present", 1, actual) == db::lookup_status::found &&
        actual == payload && ok;
    unsigned pages = 0;
    const auto streamed = database.visit_pages("present", [&](uint64_t id,
        const std::string& bytes) {
        ++pages; return id == 1 && bytes == payload;
    });
    ok = streamed == db::batch_status::ok && pages == 1 && ok;
    std::vector<std::string> keys;
    const auto scanned = database.scan(disk_address::bucket("present"), keys);
    ok = scanned == db::scan_status::ok && keys ==
        std::vector<std::string>{"present"} && ok;
    keys.clear();
    const auto visited = database.visit_keys([&](const std::string& key) {
        keys.push_back(key); return true;
    });
    ok = visited == db::scan_status::ok && keys ==
        std::vector<std::string>{"present"} && ok;
    if (ok) std::cout << "Native WT positioned read errors "
        "and absence passed\n";
    return ok ? 0 : 1;
}
