#include "remote_call.h"

namespace {
constexpr uint64_t kDummyReturnAddr = 0;
constexpr uint64_t kStackScratch = 128;
}

RemoteCallResult remote_call(PtraceSession& session,
                             uint64_t func_addr,
                             const uint64_t* args,
                             int arg_count,
                             std::string* error) {
    RemoteCallResult result{};
    result.success = false;
    result.return_value = 0;
    result.stop_signal = -1;

    if (arg_count < 0 || arg_count > 6) {
        if (error) {
            *error = "remote_call supports 0-6 args";
        }
        return result;
    }

    Arm64Regs saved{};
    if (!session.get_regs(&saved, error)) {
        return result;
    }

    Arm64Regs regs = saved;
    for (int i = 0; i < arg_count; ++i) {
        regs.x[i] = args[i];
    }

    regs.sp = (saved.sp - kStackScratch) & ~0xFULL;
    regs.pc = func_addr;
    regs.x[30] = kDummyReturnAddr;

    if (!session.set_regs(regs, error)) {
        return result;
    }

    if (!session.cont(error)) {
        session.set_regs(saved, nullptr);
        return result;
    }

    int stop_signal = -1;
    int wait_status = 0;
    if (!session.wait_for_stop(&stop_signal, &wait_status, error)) {
        session.set_regs(saved, nullptr);
        return result;
    }

    Arm64Regs out{};
    if (!session.get_regs(&out, error)) {
        session.set_regs(saved, nullptr);
        return result;
    }

    session.set_regs(saved, nullptr);

    result.stop_signal = stop_signal;
    result.return_value = out.x[0];
    result.success = (out.pc == kDummyReturnAddr);
    if (!result.success && error) {
        *error = "remote call did not return to dummy LR";
    }
    return result;
}

RemoteCallResult remote_call0(PtraceSession& session, uint64_t func_addr, std::string* error) {
    return remote_call(session, func_addr, nullptr, 0, error);
}

RemoteCallResult remote_call1(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, std::string* error) {
    const uint64_t args[] = {a0};
    return remote_call(session, func_addr, args, 1, error);
}

RemoteCallResult remote_call2(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, std::string* error) {
    const uint64_t args[] = {a0, a1};
    return remote_call(session, func_addr, args, 2, error);
}

RemoteCallResult remote_call3(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, std::string* error) {
    const uint64_t args[] = {a0, a1, a2};
    return remote_call(session, func_addr, args, 3, error);
}

RemoteCallResult remote_call4(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              std::string* error) {
    const uint64_t args[] = {a0, a1, a2, a3};
    return remote_call(session, func_addr, args, 4, error);
}

RemoteCallResult remote_call6(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, std::string* error) {
    const uint64_t args[] = {a0, a1, a2, a3, a4, a5};
    return remote_call(session, func_addr, args, 6, error);
}
