#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct Arm64Regs {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

class PtraceSession {
public:
    explicit PtraceSession(int tid);
    ~PtraceSession();

    bool attach(std::string* error);
    bool detach(std::string* error = nullptr);

    bool get_regs(Arm64Regs* regs, std::string* error) const;
    bool set_regs(const Arm64Regs& regs, std::string* error) const;

    bool read_memory(uint64_t remote_addr, void* out, size_t size, std::string* error) const;
    bool write_memory(uint64_t remote_addr, const void* data, size_t size, std::string* error) const;

    bool cont(std::string* error);
    bool wait_for_stop(int* signal, int* status, std::string* error);

    int tid() const { return tid_; }
    bool attached() const { return attached_; }
    bool used_seize() const { return used_seize_; }

private:
    bool ptrace_getregset(Arm64Regs* regs, std::string* error) const;
    bool ptrace_setregset(const Arm64Regs& regs, std::string* error) const;

    int tid_;
    bool attached_;
    bool used_seize_;
    mutable bool regset_supported_;
};
