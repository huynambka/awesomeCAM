#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <stdint.h>

namespace awesomecam {

using FnAServiceManagerAddService = binder_status_t (*)(AIBinder *binder, const char *instance);
using FnAServiceManagerWaitForService = AIBinder *(*)(const char *instance);
using FnAServiceManagerCheckService = AIBinder *(*)(const char *instance);
using FnABinderProcessSetThreadPoolMaxThreadCount = void (*)(uint32_t numThreads);
using FnABinderProcessStartThreadPool = void (*)();
using FnABinderProcessJoinThreadPool = void (*)();
using FnAIBinderClassDefine = AIBinder_Class *(*)(const char *, AIBinder_Class_onCreate,
                                                  AIBinder_Class_onDestroy,
                                                  AIBinder_Class_onTransact);
using FnAIBinderNew = AIBinder *(*)(const AIBinder_Class *, void *);
using FnAIBinderAssociateClass = bool (*)(AIBinder *, const AIBinder_Class *);
using FnAIBinderIsAlive = bool (*)(const AIBinder *);
using FnAIBinderPrepareTransaction = binder_status_t (*)(AIBinder *, AParcel **);
using FnAIBinderTransact = binder_status_t (*)(AIBinder *, transaction_code_t, AParcel **,
                                               AParcel **, binder_flags_t);
using FnAParcelDelete = void (*)(AParcel *);
using FnAParcelReadInt32 = binder_status_t (*)(const AParcel *, int32_t *);
using FnAParcelWriteInt32 = binder_status_t (*)(AParcel *, int32_t);
using FnAParcelReadInt64 = binder_status_t (*)(const AParcel *, int64_t *);
using FnAParcelWriteInt64 = binder_status_t (*)(AParcel *, int64_t);
using FnAParcelReadParcelFileDescriptor = binder_status_t (*)(const AParcel *, int *);
using FnAParcelWriteParcelFileDescriptor = binder_status_t (*)(AParcel *, int);
using FnAParcelReadByteArray = binder_status_t (*)(const AParcel *, void *,
                                                   AParcel_byteArrayAllocator);
using FnAParcelWriteByteArray = binder_status_t (*)(AParcel *, const int8_t *, int32_t);

struct BinderRuntimeApi {
  void *handle = nullptr;
  FnAServiceManagerAddService add_service = nullptr;
  FnAServiceManagerWaitForService wait_for_service = nullptr;
  FnAServiceManagerCheckService check_service = nullptr;
  FnABinderProcessSetThreadPoolMaxThreadCount set_thread_pool_max = nullptr;
  FnABinderProcessStartThreadPool start_thread_pool = nullptr;
  FnABinderProcessJoinThreadPool join_thread_pool = nullptr;
  FnAIBinderClassDefine binder_class_define = nullptr;
  FnAIBinderNew binder_new = nullptr;
  FnAIBinderAssociateClass binder_associate_class = nullptr;
  FnAIBinderIsAlive binder_is_alive = nullptr;
  FnAIBinderPrepareTransaction binder_prepare_transaction = nullptr;
  FnAIBinderTransact binder_transact = nullptr;
  FnAParcelDelete parcel_delete = nullptr;
  FnAParcelReadInt32 parcel_read_int32 = nullptr;
  FnAParcelWriteInt32 parcel_write_int32 = nullptr;
  FnAParcelReadInt64 parcel_read_int64 = nullptr;
  FnAParcelWriteInt64 parcel_write_int64 = nullptr;
  FnAParcelReadParcelFileDescriptor parcel_read_fd = nullptr;
  FnAParcelWriteParcelFileDescriptor parcel_write_fd = nullptr;
  FnAParcelReadByteArray parcel_read_byte_array = nullptr;
  FnAParcelWriteByteArray parcel_write_byte_array = nullptr;
};

bool LoadBinderRuntimeApi(BinderRuntimeApi *api);

}  // namespace awesomecam
