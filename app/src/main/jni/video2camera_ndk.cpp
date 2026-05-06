#include "video2camera_ndk.h"

#include <dlfcn.h>
#include <type_traits>

namespace awesomecam {

bool LoadBinderRuntimeApi(BinderRuntimeApi *api) {
  if (api == nullptr) return false;
  if (api->handle != nullptr) {
    return api->add_service != nullptr && api->wait_for_service != nullptr &&
           api->check_service != nullptr && api->set_thread_pool_max != nullptr &&
           api->start_thread_pool != nullptr && api->join_thread_pool != nullptr &&
           api->binder_class_define != nullptr && api->binder_new != nullptr &&
           api->binder_associate_class != nullptr && api->binder_is_alive != nullptr &&
           api->binder_prepare_transaction != nullptr && api->binder_transact != nullptr &&
           api->parcel_delete != nullptr && api->parcel_read_int32 != nullptr &&
           api->parcel_write_int32 != nullptr && api->parcel_read_int64 != nullptr &&
           api->parcel_write_int64 != nullptr && api->parcel_read_fd != nullptr &&
           api->parcel_write_fd != nullptr && api->parcel_read_byte_array != nullptr &&
           api->parcel_write_byte_array != nullptr;
  }

  api->handle = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
  if (api->handle == nullptr) {
    return false;
  }

  auto sym = [&](auto &slot, const char *name) {
    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(api->handle, name));
    return slot != nullptr;
  };

  bool ok = true;
  ok &= sym(api->add_service, "AServiceManager_addService");
  ok &= sym(api->wait_for_service, "AServiceManager_waitForService");
  ok &= sym(api->check_service, "AServiceManager_checkService");
  ok &= sym(api->set_thread_pool_max, "ABinderProcess_setThreadPoolMaxThreadCount");
  ok &= sym(api->start_thread_pool, "ABinderProcess_startThreadPool");
  ok &= sym(api->join_thread_pool, "ABinderProcess_joinThreadPool");
  ok &= sym(api->binder_class_define, "AIBinder_Class_define");
  ok &= sym(api->binder_new, "AIBinder_new");
  ok &= sym(api->binder_associate_class, "AIBinder_associateClass");
  ok &= sym(api->binder_is_alive, "AIBinder_isAlive");
  ok &= sym(api->binder_prepare_transaction, "AIBinder_prepareTransaction");
  ok &= sym(api->binder_transact, "AIBinder_transact");
  ok &= sym(api->parcel_delete, "AParcel_delete");
  ok &= sym(api->parcel_read_int32, "AParcel_readInt32");
  ok &= sym(api->parcel_write_int32, "AParcel_writeInt32");
  ok &= sym(api->parcel_read_int64, "AParcel_readInt64");
  ok &= sym(api->parcel_write_int64, "AParcel_writeInt64");
  ok &= sym(api->parcel_read_fd, "AParcel_readParcelFileDescriptor");
  ok &= sym(api->parcel_write_fd, "AParcel_writeParcelFileDescriptor");
  ok &= sym(api->parcel_read_byte_array, "AParcel_readByteArray");
  ok &= sym(api->parcel_write_byte_array, "AParcel_writeByteArray");

  return ok;
}

}  // namespace awesomecam
