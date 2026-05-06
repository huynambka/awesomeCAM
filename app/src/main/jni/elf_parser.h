#pragma once

#include <cstdint>
#include <optional>
#include <string>

std::optional<uint64_t> find_elf_symbol_value(const std::string& elf_path, const std::string& symbol_name);
std::optional<uint64_t> find_elf_min_load_vaddr(const std::string& elf_path);
std::optional<std::string> resolve_local_linker_path();
std::optional<std::string> resolve_local_libc_path();
