#pragma once

#include "ptrace_utils.h"

#include <cstdint>
#include <string>

struct RemoteCallResult {
    bool success;
    uint64_t return_value;
    int stop_signal;
};

RemoteCallResult remote_call(PtraceSession& session,
                             uint64_t func_addr,
                             const uint64_t* args,
                             int arg_count,
                             std::string* error);
RemoteCallResult remote_call0(PtraceSession& session, uint64_t func_addr, std::string* error);
RemoteCallResult remote_call1(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, std::string* error);
RemoteCallResult remote_call2(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, std::string* error);
RemoteCallResult remote_call3(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, std::string* error);
RemoteCallResult remote_call4(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              std::string* error);
RemoteCallResult remote_call6(PtraceSession& session, uint64_t func_addr,
                              uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, std::string* error);
