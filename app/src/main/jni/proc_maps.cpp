#include "proc_maps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

std::vector<ModuleInfo> parse_proc_maps(int pid) {
    std::vector<ModuleInfo> maps;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return maps;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[8] = {0};
        char file_path[512] = {0};

        int matched = sscanf(line,
                             "%llx-%llx %7s %*s %*s %*s %511[^\n]",
                             &start,
                             &end,
                             perms,
                             file_path);
        if (matched < 3) {
            continue;
        }

        ModuleInfo info{};
        info.base = static_cast<uint64_t>(start);
        info.end = static_cast<uint64_t>(end);
        info.is_rx = (strchr(perms, 'r') != nullptr) && (strchr(perms, 'x') != nullptr);
        if (matched >= 4) {
            info.path = file_path;
            while (!info.path.empty() && info.path.front() == ' ') {
                info.path.erase(info.path.begin());
            }
        }

        maps.push_back(std::move(info));
    }

    fclose(fp);
    return maps;
}

const ModuleInfo* find_module(const std::vector<ModuleInfo>& maps, const std::string& substr) {
    for (const auto& map : maps) {
        if (!map.path.empty() && map.path.find(substr) != std::string::npos) {
            return &map;
        }
    }
    return nullptr;
}

const ModuleInfo* find_rx_module(const std::vector<ModuleInfo>& maps, const std::string& substr) {
    for (const auto& map : maps) {
        if (map.is_rx && !map.path.empty() && map.path.find(substr) != std::string::npos) {
            return &map;
        }
    }
    return nullptr;
}

const ModuleInfo* find_mapping_for_addr(const std::vector<ModuleInfo>& maps, uint64_t addr) {
    for (const auto& map : maps) {
        if (addr >= map.base && addr < map.end) {
            return &map;
        }
    }
    return nullptr;
}

std::vector<int> get_thread_tids(int pid) {
    std::vector<int> tids;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);

    DIR* dir = opendir(path);
    if (!dir) {
        return tids;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        int tid = atoi(entry->d_name);
        if (tid > 0) {
            tids.push_back(tid);
        }
    }

    closedir(dir);
    std::sort(tids.begin(), tids.end());
    return tids;
}
