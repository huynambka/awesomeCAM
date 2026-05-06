#include "ptrace_utils.h"

#include <asm/ptrace.h>
#include <elf.h>
#include <errno.h>
#include <linux/ptrace.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static std::string ptrace_error(int req, int tid, int errnum) {
    char buf[256];
    snprintf(buf, sizeof(buf), "ptrace(%d, %d) failed: %s", req, tid, strerror(errnum));
    return std::string(buf);
}

PtraceSession::PtraceSession(int tid)
    : tid_(tid), attached_(false), used_seize_(false), regset_supported_(true) {}

PtraceSession::~PtraceSession() {
    if (attached_) {
        detach(nullptr);
    }
}

bool PtraceSession::attach(std::string* error) {
    long options = PTRACE_O_TRACESYSGOOD;
    long ret = ptrace(PTRACE_SEIZE, tid_, nullptr, reinterpret_cast<void*>(options));
    if (ret == 0) {
        used_seize_ = true;
        ret = ptrace(PTRACE_INTERRUPT, tid_, nullptr, nullptr);
        if (ret != 0) {
            int e = errno;
            if (error) {
                *error = ptrace_error(PTRACE_INTERRUPT, tid_, e);
            }
            ptrace(PTRACE_DETACH, tid_, nullptr, nullptr);
            return false;
        }

        int status = 0;
        int wait_ret = waitpid(tid_, &status, 0);
        if (wait_ret != tid_) {
            if (error) {
                *error = "waitpid after SEIZE+INTERRUPT failed";
            }
            ptrace(PTRACE_DETACH, tid_, nullptr, nullptr);
            return false;
        }
    } else {
        ret = ptrace(PTRACE_ATTACH, tid_, nullptr, nullptr);
        if (ret != 0) {
            int e = errno;
            if (error) {
                *error = ptrace_error(PTRACE_ATTACH, tid_, e);
            }
            return false;
        }

        int status = 0;
        int wait_ret = waitpid(tid_, &status, 0);
        if (wait_ret != tid_) {
            if (error) {
                *error = "waitpid after ATTACH failed";
            }
            return false;
        }

        ptrace(PTRACE_SETOPTIONS, tid_, nullptr, reinterpret_cast<void*>(options));
        used_seize_ = false;
    }

    attached_ = true;
    return true;
}

bool PtraceSession::detach(std::string* error) {
    if (!attached_) {
        return true;
    }

    long ret = ptrace(PTRACE_DETACH, tid_, nullptr, nullptr);
    if (ret != 0 && error != nullptr) {
        *error = ptrace_error(PTRACE_DETACH, tid_, errno);
    }
    attached_ = false;
    return ret == 0;
}

bool PtraceSession::ptrace_getregset(Arm64Regs* regs, std::string* error) const {
    struct iovec iov{};
    struct user_pt_regs pt_regs{};
    iov.iov_base = &pt_regs;
    iov.iov_len = sizeof(pt_regs);

    long ret = ptrace(PTRACE_GETREGSET, tid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov);
    if (ret != 0) {
        int e = errno;
        if (e == EPERM || e == ESRCH) {
            if (error) {
                *error = ptrace_error(PTRACE_GETREGSET, tid_, e);
            }
            return false;
        }
        return false;
    }

    for (int i = 0; i < 31; ++i) {
        regs->x[i] = pt_regs.regs[i];
    }
    regs->sp = pt_regs.sp;
    regs->pc = pt_regs.pc;
    regs->pstate = pt_regs.pstate;
    return true;
}

bool PtraceSession::ptrace_setregset(const Arm64Regs& regs, std::string* error) const {
    struct user_pt_regs pt_regs{};
    for (int i = 0; i < 31; ++i) {
        pt_regs.regs[i] = regs.x[i];
    }
    pt_regs.sp = regs.sp;
    pt_regs.pc = regs.pc;
    pt_regs.pstate = regs.pstate;

    struct iovec iov{};
    iov.iov_base = &pt_regs;
    iov.iov_len = sizeof(pt_regs);

    long ret = ptrace(PTRACE_SETREGSET, tid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov);
    if (ret != 0) {
        if (error) {
            *error = ptrace_error(PTRACE_SETREGSET, tid_, errno);
        }
        return false;
    }
    return true;
}

bool PtraceSession::get_regs(Arm64Regs* regs, std::string* error) const {
    if (regset_supported_) {
        if (ptrace_getregset(regs, error)) {
            return true;
        }
        regset_supported_ = false;
    }
    return false;
}

bool PtraceSession::set_regs(const Arm64Regs& regs, std::string* error) const {
    if (regset_supported_) {
        if (ptrace_setregset(regs, error)) {
            return true;
        }
        regset_supported_ = false;
    }
    return false;
}

bool PtraceSession::read_memory(uint64_t remote_addr, void* out, size_t size, std::string* error) const {
    auto* dst = static_cast<uint8_t*>(out);
    size_t offset = 0;
    constexpr size_t word_size = sizeof(long);

    while (offset < size) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, tid_, reinterpret_cast<void*>(remote_addr + offset), nullptr);
        if (errno != 0) {
            if (error) {
                char buf[256];
                snprintf(buf, sizeof(buf), "PEEKDATA at 0x%lx failed: %s",
                         static_cast<unsigned long>(remote_addr + offset), strerror(errno));
                *error = buf;
            }
            return false;
        }
        size_t chunk = std::min(size - offset, word_size);
        memcpy(dst + offset, &word, chunk);
        offset += chunk;
    }
    return true;
}

bool PtraceSession::write_memory(uint64_t remote_addr, const void* data, size_t size, std::string* error) const {
    auto* src = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    constexpr size_t word_size = sizeof(long);

    while (offset < size) {
        long word = 0;
        size_t chunk = std::min(size - offset, word_size);
        if (chunk < word_size) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, tid_, reinterpret_cast<void*>(remote_addr + offset), nullptr);
            if (errno != 0) {
                if (error) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "PEEKDATA for RMW at 0x%lx failed: %s",
                             static_cast<unsigned long>(remote_addr + offset), strerror(errno));
                    *error = buf;
                }
                return false;
            }
        }
        memcpy(&word, src + offset, chunk);

        long ret = ptrace(PTRACE_POKEDATA, tid_, reinterpret_cast<void*>(remote_addr + offset),
                          reinterpret_cast<void*>(word));
        if (ret != 0) {
            if (error) {
                char buf[256];
                snprintf(buf, sizeof(buf), "POKEDATA at 0x%lx failed: %s",
                         static_cast<unsigned long>(remote_addr + offset), strerror(errno));
                *error = buf;
            }
            return false;
        }
        offset += chunk;
    }
    return true;
}

bool PtraceSession::cont(std::string* error) {
    long ret = ptrace(PTRACE_CONT, tid_, nullptr, nullptr);
    if (ret != 0) {
        if (error) {
            *error = ptrace_error(PTRACE_CONT, tid_, errno);
        }
        return false;
    }
    return true;
}

bool PtraceSession::wait_for_stop(int* signal, int* status, std::string* error) {
    int st = 0;
    int wait_ret = waitpid(tid_, &st, 0);
    if (wait_ret != tid_) {
        if (error) {
            *error = "waitpid failed";
        }
        return false;
    }
    if (status) {
        *status = st;
    }
    if (WIFSTOPPED(st)) {
        if (signal) {
            *signal = WSTOPSIG(st);
        }
    } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
        if (error) {
            *error = "target thread exited during remote call";
        }
        return false;
    }
    return true;
}
