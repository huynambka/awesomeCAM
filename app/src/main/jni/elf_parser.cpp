#include "elf_parser.h"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <optional>
#include <string>

struct ElfFile {
    int fd = -1;
    off_t size = 0;
    void* data = nullptr;

    ~ElfFile() {
        if (data != nullptr && data != MAP_FAILED) {
            munmap(data, size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        struct stat st{};
        if (fstat(fd, &st) != 0) {
            return false;
        }
        size = st.st_size;
        data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        return data != MAP_FAILED;
    }
};

static bool open_elf64(const std::string& path, ElfFile* elf, Elf64_Ehdr** ehdr_out) {
    if (elf == nullptr || ehdr_out == nullptr) return false;
    if (!elf->open(path)) return false;
    auto* ehdr = static_cast<Elf64_Ehdr*>(elf->data);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }
    *ehdr_out = ehdr;
    return true;
}

std::optional<uint64_t> find_elf_symbol_value(const std::string& elf_path, const std::string& symbol_name) {
    ElfFile elf{};
    Elf64_Ehdr* ehdr = nullptr;
    if (!open_elf64(elf_path, &elf, &ehdr)) {
        return std::nullopt;
    }

    auto* shdrs = reinterpret_cast<Elf64_Shdr*>(static_cast<uint8_t*>(elf.data) + ehdr->e_shoff);
    int shnum = ehdr->e_shnum;

    const Elf64_Shdr* dynsym_sh = nullptr;
    const Elf64_Shdr* dynstr_sh = nullptr;

    for (int i = 0; i < shnum; ++i) {
        if (shdrs[i].sh_type == SHT_DYNSYM) {
            dynsym_sh = &shdrs[i];
            dynstr_sh = &shdrs[shdrs[i].sh_link];
            break;
        }
    }

    if (!dynsym_sh || !dynstr_sh) {
        return std::nullopt;
    }

    auto* strtab = static_cast<const char*>(elf.data) + dynstr_sh->sh_offset;
    auto* syms = reinterpret_cast<const Elf64_Sym*>(static_cast<uint8_t*>(elf.data) + dynsym_sh->sh_offset);
    int sym_count = static_cast<int>(dynsym_sh->sh_size / dynsym_sh->sh_entsize);

    for (int i = 0; i < sym_count; ++i) {
        const char* name = strtab + syms[i].st_name;
        if (strcmp(name, symbol_name.c_str()) == 0) {
            return syms[i].st_value;
        }
    }

    return std::nullopt;
}

std::optional<uint64_t> find_elf_min_load_vaddr(const std::string& elf_path) {
    ElfFile elf{};
    Elf64_Ehdr* ehdr = nullptr;
    if (!open_elf64(elf_path, &elf, &ehdr)) {
        return std::nullopt;
    }

    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(static_cast<uint8_t*>(elf.data) + ehdr->e_phoff);
    uint64_t min_vaddr = UINT64_MAX;
    bool found = false;
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        min_vaddr = std::min<uint64_t>(min_vaddr, phdrs[i].p_vaddr);
        found = true;
    }
    if (!found) return std::nullopt;
    return min_vaddr & ~static_cast<uint64_t>(0xfff);
}

std::optional<std::string> resolve_local_linker_path() {
    const char* candidates[] = {
        "/apex/com.android.runtime/bin/linker64",
        "/system/bin/linker64",
    };
    for (const auto* p : candidates) {
        if (access(p, R_OK) == 0) {
            return std::string(p);
        }
    }
    return std::nullopt;
}

std::optional<std::string> resolve_local_libc_path() {
    const char* candidates[] = {
        "/apex/com.android.runtime/lib64/bionic/libc.so",
        "/system/lib64/libc.so",
        "/system/lib64/bionic/libc.so",
    };
    for (const auto* p : candidates) {
        if (access(p, R_OK) == 0) {
            return std::string(p);
        }
    }
    return std::nullopt;
}
