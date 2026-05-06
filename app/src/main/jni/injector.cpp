#include "injector.h"
#include "elf_parser.h"
#include "proc_maps.h"
#include "ptrace_utils.h"
#include "remote_call.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#define LOG_TAG "awesomeCAM"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define info_log(fmt, ...) do { \
    ALOGI(fmt, ##__VA_ARGS__); \
    fprintf(stdout, "[+] " fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

#define err_log(fmt, ...) do { \
    ALOGE(fmt, ##__VA_ARGS__); \
    fprintf(stderr, "[-] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
} while(0)

namespace {

const char* kLinkerPatterns[] = {
    "/linker64",
    "/apex/com.android.runtime/bin/linker64",
    "/system/bin/linker64",
};

const char* kLibcPatterns[] = {
    "/libc.so",
};

int find_pid_by_name(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pidof %s", name);
    FILE* fp = popen(cmd, "r");
    if (fp == nullptr) {
        return -1;
    }

    char buf[128] = {0};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    if (n == 0) {
        return -1;
    }

    return atoi(buf);
}

std::string basename_copy(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

const ModuleInfo* find_loaded_module_for_path(const std::vector<ModuleInfo>& maps,
                                              const std::string& path) {
    for (const auto& map : maps) {
        if (map.path == path) {
            return &map;
        }
    }

    const std::string base = basename_copy(path);
    for (const auto& map : maps) {
        if (!map.path.empty() && basename_copy(map.path) == base) {
            return &map;
        }
    }
    return nullptr;
}

bool maps_contains_path(int pid, const std::string& path) {
    if (pid <= 0 || path.empty()) {
        return false;
    }
    auto maps = parse_proc_maps(pid);
    return find_loaded_module_for_path(maps, path) != nullptr;
}

bool process_alive(int pid) {
    if (pid <= 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    return access(path, F_OK) == 0;
}

}  // namespace

int do_inject(const char* target_name, const char* loader_path, const char* export_symbol) {
    if (target_name == nullptr || target_name[0] == '\0') {
        err_log("target process name is empty");
        return 1;
    }
    if (loader_path == nullptr || loader_path[0] != '/') {
        err_log("loader path must be absolute (got %s)", loader_path != nullptr ? loader_path : "(null)");
        return 1;
    }

    info_log("Start native inject [%s -> %s%s%s]",
             target_name,
             loader_path,
             (export_symbol != nullptr && export_symbol[0] != '\0') ? " ; call " : "",
             (export_symbol != nullptr && export_symbol[0] != '\0') ? export_symbol : "");

    const int pid = find_pid_by_name(target_name);
    if (pid <= 0) {
        err_log("target process %s not found", target_name);
        return 1;
    }
    info_log("Resolved %s pid=%d", target_name, pid);

    auto maps = parse_proc_maps(pid);
    if (maps.empty()) {
        err_log("failed to parse /proc/%d/maps", pid);
        return 1;
    }

    const ModuleInfo* remote_libc = nullptr;
    for (const auto* pat : kLibcPatterns) {
        remote_libc = find_module(maps, pat);
        if (remote_libc != nullptr) break;
    }
    if (remote_libc == nullptr) {
        err_log("remote libc not found in maps");
        return 1;
    }
    info_log("Remote libc: %s @ 0x%llx", remote_libc->path.c_str(),
             static_cast<unsigned long long>(remote_libc->base));

    const ModuleInfo* remote_linker = nullptr;
    for (const auto* pat : kLinkerPatterns) {
        remote_linker = find_module(maps, pat);
        if (remote_linker != nullptr) break;
    }
    if (remote_linker == nullptr) {
        err_log("remote linker64 not found in maps");
        return 1;
    }
    info_log("Remote linker: %s @ 0x%llx", remote_linker->path.c_str(),
             static_cast<unsigned long long>(remote_linker->base));

    const ModuleInfo* target_rx = find_rx_module(maps, target_name);
    if (target_rx == nullptr) {
        for (const auto& map : maps) {
            if (map.is_rx && !map.path.empty()) {
                target_rx = &map;
                break;
            }
        }
    }
    if (target_rx == nullptr) {
        err_log("no executable mapping found for caller_addr");
        return 1;
    }
    const uint64_t caller_addr = target_rx->base;
    info_log("caller_addr: 0x%llx (%s)", static_cast<unsigned long long>(caller_addr),
             target_rx->path.c_str());

    const auto local_libc_path = resolve_local_libc_path();
    if (!local_libc_path) {
        err_log("local libc not found on device");
        return 1;
    }
    const auto local_linker_path = resolve_local_linker_path();
    if (!local_linker_path) {
        err_log("local linker64 not found on device");
        return 1;
    }

    const auto mmap_offset = find_elf_symbol_value(*local_libc_path, "mmap");
    if (!mmap_offset) {
        err_log("symbol 'mmap' not found in local libc");
        return 1;
    }

    const char* dlopen_names[] = {
        "__loader_dlopen",
        "__dl__Z8__dlopenPKciPKv",
        "dlopen",
    };
    std::optional<uint64_t> dlopen_offset;
    const char* dlopen_name_used = nullptr;
    for (const auto* name : dlopen_names) {
        dlopen_offset = find_elf_symbol_value(*local_linker_path, name);
        if (dlopen_offset) {
            dlopen_name_used = name;
            break;
        }
    }
    if (!dlopen_offset) {
        err_log("dlopen symbol not found in local linker");
        return 1;
    }

    const uint64_t remote_mmap = remote_libc->base + *mmap_offset;
    const uint64_t remote_dlopen = remote_linker->base + *dlopen_offset;
    info_log("mmap offset: 0x%llx", static_cast<unsigned long long>(*mmap_offset));
    info_log("dlopen (%s) offset: 0x%llx", dlopen_name_used,
             static_cast<unsigned long long>(*dlopen_offset));
    info_log("remote mmap: 0x%llx", static_cast<unsigned long long>(remote_mmap));
    info_log("remote dlopen: 0x%llx", static_cast<unsigned long long>(remote_dlopen));

    const auto tids = get_thread_tids(pid);
    if (tids.empty()) {
        err_log("no threads found for pid %d", pid);
        return 1;
    }

    const int tid = tids[0];
    info_log("Attaching to tid %d", tid);

    std::string err;
    PtraceSession session(tid);
    if (!session.attach(&err)) {
        err_log("ptrace attach failed: %s", err.c_str());
        return 1;
    }
    info_log("Attached (seize=%d)", session.used_seize() ? 1 : 0);

    const size_t path_size = strlen(loader_path) + 1;
    auto mmap_result = remote_call6(session, remote_mmap,
                                    0,
                                    path_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS,
                                    static_cast<uint64_t>(-1),
                                    0,
                                    &err);
    if (!mmap_result.success || mmap_result.return_value == 0 ||
        mmap_result.return_value == reinterpret_cast<uint64_t>(MAP_FAILED)) {
        err_log("remote mmap failed: %s", err.c_str());
        return 1;
    }
    const uint64_t remote_path_addr = mmap_result.return_value;
    info_log("remote mmap returned 0x%llx", static_cast<unsigned long long>(remote_path_addr));

    if (!session.write_memory(remote_path_addr, loader_path, path_size, &err)) {
        err_log("write_memory failed: %s", err.c_str());
        return 1;
    }
    info_log("Wrote path to 0x%llx", static_cast<unsigned long long>(remote_path_addr));

    const int dlopen_flags = RTLD_NOW | RTLD_GLOBAL;
    auto dlopen_result = remote_call3(session, remote_dlopen,
                                      remote_path_addr,
                                      static_cast<uint64_t>(dlopen_flags),
                                      caller_addr,
                                      &err);
    if (!dlopen_result.success) {
        err_log("remote dlopen call failed: %s", err.c_str());
        return 1;
    }
    if (dlopen_result.return_value == 0) {
        err_log("dlopen returned NULL for %s", loader_path);
        return 1;
    }
    info_log("dlopen returned handle 0x%llx", static_cast<unsigned long long>(dlopen_result.return_value));

    maps = parse_proc_maps(pid);
    const ModuleInfo* loaded = find_loaded_module_for_path(maps, loader_path);
    if (loaded == nullptr) {
        err_log("payload %s not present in maps after dlopen", loader_path);
        return 1;
    }
    info_log("Payload mapped at 0x%llx (%s)", static_cast<unsigned long long>(loaded->base), loaded->path.c_str());

    if (export_symbol != nullptr && export_symbol[0] != '\0') {
        const auto symbol_value = find_elf_symbol_value(loader_path, export_symbol);
        if (!symbol_value) {
            err_log("export %s not found in %s", export_symbol, loader_path);
            return 1;
        }
        const auto min_vaddr = find_elf_min_load_vaddr(loader_path);
        const uint64_t load_bias = min_vaddr.value_or(0);
        const uint64_t remote_symbol = loaded->base + *symbol_value - load_bias;
        info_log("Calling export %s at 0x%llx (symbol=0x%llx load_bias=0x%llx)",
                 export_symbol,
                 static_cast<unsigned long long>(remote_symbol),
                 static_cast<unsigned long long>(*symbol_value),
                 static_cast<unsigned long long>(load_bias));
        auto call_result = remote_call0(session, remote_symbol, &err);
        if (!call_result.success) {
            err_log("remote call %s failed: %s", export_symbol, err.c_str());
            return 1;
        }
        info_log("Export %s returned 0x%llx", export_symbol,
                 static_cast<unsigned long long>(call_result.return_value));
    }

    if (!session.detach(&err)) {
        err_log("ptrace detach failed: %s", err.c_str());
        return 1;
    }
    info_log("Detached from tid %d", tid);

    usleep(250 * 1000);

    if (!process_alive(pid)) {
        const int new_pid = find_pid_by_name(target_name);
        err_log("target pid %d disappeared after inject; current %s pid=%d", pid, target_name, new_pid);
        return 1;
    }

    if (!maps_contains_path(pid, loader_path)) {
        err_log("payload %s missing from /proc/%d/maps after detach", loader_path, pid);
        return 1;
    }

    info_log("Verified payload still mapped after detach: %s", loader_path);
    info_log("Finish Inject");
    return 0;
}
