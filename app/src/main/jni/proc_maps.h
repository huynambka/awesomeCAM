#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ModuleInfo {
    uint64_t base;
    uint64_t end;
    std::string path;
    bool is_rx;
};

std::vector<ModuleInfo> parse_proc_maps(int pid);
const ModuleInfo* find_module(const std::vector<ModuleInfo>& maps, const std::string& substr);
const ModuleInfo* find_rx_module(const std::vector<ModuleInfo>& maps, const std::string& substr);
const ModuleInfo* find_mapping_for_addr(const std::vector<ModuleInfo>& maps, uint64_t addr);
std::vector<int> get_thread_tids(int pid);
