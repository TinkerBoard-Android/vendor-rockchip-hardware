/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <android/log.h>

#include <sys/system_properties.h>

#include "NeuralNetworksTypes.h"
#include "rknn_api.h"

#define TAG "RKNN_API"
// #define NNAPI_LOG(...) __android_log_print(ANDROID_LOG_ERROR,TAG ,__VA_ARGS__)
#define NNAPI_LOG(...)

struct NnApi {
  bool nnapi_exists;
  int32_t android_sdk_version;

  int (*ARKNN_client_create)(ARKNNHAL **hal);
  int (*ARKNN_find_devices)(ARKNNHAL *hal, rknn_devices_id* pdevs);
  int (*ARKNN_init)(ARKNNHAL *hal, rknn_context *context, void *model, uint32_t size, uint32_t flag);
  int (*ARKNN_init2)(ARKNNHAL *hal, rknn_context* context, void *model, uint32_t size, uint32_t flag, rknn_init_extend* extend);
  int (*ARKNN_destroy)(ARKNNHAL *hal, rknn_context);
  int (*ARKNN_query)(ARKNNHAL *hal, rknn_context context, rknn_query_cmd cmd, void* info, uint32_t size);
  int (*ARKNN_inputs_set)(ARKNNHAL *hal, rknn_context context, uint32_t n_inputs, rknn_input inputs[]);
  int (*ARKNN_run)(ARKNNHAL *hal, rknn_context context, rknn_run_extend* extend);
  int (*ARKNN_outputs_get)(ARKNNHAL *hal, rknn_context, uint32_t n_outputs, rknn_output outputs[], rknn_output_extend* extend);
  int (*ARKNN_outputs_release)(ARKNNHAL *hal, rknn_context context, uint32_t n_ouputs, rknn_output outputs[]);

  int (*ASharedMemory_create)(const char* name, size_t size);
};

static int32_t GetAndroidSdkVersion() {
  const char* sdkProp = "ro.build.version.sdk";
  char sdkVersion[PROP_VALUE_MAX];
  int length = __system_property_get(sdkProp, sdkVersion);
  if (length != 0) {
    int32_t result = 0;
    for (int i = 0; i < length; ++i) {
      int digit = sdkVersion[i] - '0';
      if (digit < 0 || digit > 9) {
        // Non-numeric SDK version, assume it's higher than expected;
        return 0xffff;
      }
      result = result * 10 + digit;
    }
    // TODO(levp): remove once SDK gets updated to 29th level
    // Upgrade SDK version for pre-release Q to be able to test functionality
    // available from SDK level 29.
    if (result == 28) {
      char versionCodename[PROP_VALUE_MAX];
      const char* versionCodenameProp = "ro.build.version.codename";
      length = __system_property_get(versionCodenameProp, versionCodename);
      if (length != 0) {
        if (versionCodename[0] == 'Q') {
          return 29;
        }
      }
    }
    return result;
  }
  return 0;
}

static void* LoadFunction(void* handle, const char* name, bool optional) {
  if (handle == nullptr) {
    return nullptr;
  }
  void* fn = dlsym(handle, name);
  if (fn == nullptr && !optional) {
    NNAPI_LOG("nnapi error: unable to open function %s", name);
  }
  return fn;
}

#define LOAD_FUNCTION(handle, name)         \
  nnapi.name = reinterpret_cast<name##_fn>( \
      LoadFunction(handle, #name, /*optional*/ false));

#define LOAD_FUNCTION_OPTIONAL(handle, name) \
  nnapi.name = reinterpret_cast<name##_fn>(  \
      LoadFunction(handle, #name, /*optional*/ true));

#define LOAD_FUNCTION_RENAME(handle, name, symbol) \
  nnapi.name = reinterpret_cast<name##_fn>(        \
      LoadFunction(handle, symbol, /*optional*/ false));

static const NnApi LoadNnApi() {
  NnApi nnapi = {};
  nnapi.android_sdk_version = 0;

  nnapi.android_sdk_version = GetAndroidSdkVersion();
  if (nnapi.android_sdk_version < 27) {
    NNAPI_LOG("nnapi error: requires android sdk version to be at least %d",
              27);
    nnapi.nnapi_exists = false;
    return nnapi;
  }

  void* librknnhal_bridge = nullptr;
  // TODO(b/123243014): change RTLD_LOCAL? Assumes there can be multiple
  // instances of nn api RT
  librknnhal_bridge = dlopen("librknnhal_bridge.so", RTLD_LAZY | RTLD_LOCAL);
  if (librknnhal_bridge == nullptr) {
    NNAPI_LOG("nnapi error: unable to open library %s", "librknnhal_bridge.so");
  }

  nnapi.nnapi_exists = librknnhal_bridge != nullptr;

  // API 27 (NN 1.0) methods.
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_client_create);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_find_devices);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_init);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_init2);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_destroy);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_query);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_inputs_set);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_run);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_outputs_get);
  LOAD_FUNCTION(librknnhal_bridge, ARKNN_outputs_release);

  /* Not sure this func exist.
   * LOAD_FUNCTION_OPTIONAL(
      librknnhal_bridge,
      ANeuralNetworksModel_setOperandSymmPerChannelQuantParams);*/

  // ASharedMemory_create has different implementations in Android depending on
  // the partition. Generally it can be loaded from libandroid.so but in vendor
  // partition (e.g. if a HAL wants to use NNAPI) it is only accessible through
  // libcutils.
  void* libandroid = nullptr;
  libandroid = dlopen("libandroid.so", RTLD_LAZY | RTLD_LOCAL);
  if (libandroid != nullptr) {
    LOAD_FUNCTION(libandroid, ASharedMemory_create);
  } else {
    void* cutils_handle = dlopen("libcutils.so", RTLD_LAZY | RTLD_LOCAL);
    if (cutils_handle != nullptr) {
      LOAD_FUNCTION_RENAME(cutils_handle, ASharedMemory_create,
                           "ashmem_create_region");
    } else {
      NNAPI_LOG("nnapi error: unable to open neither libraries %s and %s",
                "libandroid.so", "libcutils.so");
    }
  }
  return nnapi;
}

static const NnApi* NnApiImplementation() {
  static const NnApi nnapi = LoadNnApi();
  return &nnapi;
}

typedef struct {
  ARKNNHAL *hal;
  rknn_context rknn_ctx;
} _rknn_context;

int rknn_find_devices(rknn_devices_id* pdevs) {
  const NnApi *_nnapi = NnApiImplementation();
  ARKNNHAL *_hal;
  _nnapi->ARKNN_client_create(&_hal);
  if (!_hal) {
      NNAPI_LOG("Failed to create RKNN HAL Client!");
      return RKNN_ERR_DEVICE_UNAVAILABLE;
  }
  return _nnapi->ARKNN_find_devices(_hal, pdevs);
}

int rknn_init2(rknn_context* context, void* model, uint32_t size, uint32_t flag, rknn_init_extend* extend) {
  int ret;
  const NnApi *_nnapi = NnApiImplementation();

  ARKNNHAL *_hal;
  _nnapi->ARKNN_client_create(&_hal);
  if (!_hal) {
      NNAPI_LOG("Failed to create RKNN HAL Client!");
      return RKNN_ERR_DEVICE_UNAVAILABLE;
  }

  rknn_context _rknn_ctx;
  if (extend != NULL) {
    ret = _nnapi->ARKNN_init2(_hal, &_rknn_ctx, model, size, flag, extend);
  } else {
    ret = _nnapi->ARKNN_init(_hal, &_rknn_ctx, model, size, flag);
  }

  if (ret == RKNN_SUCC) {
    _rknn_context *_ctx = (_rknn_context *)malloc(sizeof(_rknn_context));
    _ctx->hal = _hal;
    _ctx->rknn_ctx = _rknn_ctx;
    *context = (rknn_context)_ctx;
  }

  return ret;
}

int rknn_init(rknn_context* context, void* model, uint32_t size, uint32_t flag) {
  return rknn_init2(context, model, size, flag, NULL);
}

int rknn_destroy(rknn_context context) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_destroy(_ctx->hal, _ctx->rknn_ctx);
}

int rknn_query(rknn_context context, rknn_query_cmd cmd, void* info, uint32_t size) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_query(_ctx->hal, _ctx->rknn_ctx, cmd, info, size);
}

int rknn_inputs_set(rknn_context context, uint32_t n_inputs, rknn_input inputs[]) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_inputs_set(_ctx->hal, _ctx->rknn_ctx, n_inputs, inputs);
}

int rknn_run(rknn_context context, rknn_run_extend* extend) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_run(_ctx->hal, _ctx->rknn_ctx, extend);
}

int rknn_outputs_get(rknn_context context, uint32_t n_outputs, rknn_output outputs[], rknn_output_extend* extend) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_outputs_get(_ctx->hal, _ctx->rknn_ctx, n_outputs, outputs, extend);
}

int rknn_outputs_release(rknn_context context, uint32_t n_ouputs, rknn_output outputs[]) {
  _rknn_context *_ctx = (_rknn_context *)context;
  if (_ctx == NULL || _ctx->hal == NULL || _ctx->rknn_ctx == NULL) {
    return RKNN_ERR_CTX_INVALID;
  }

  const NnApi *_nnapi = NnApiImplementation();

  return _nnapi->ARKNN_outputs_release(_ctx->hal, _ctx->rknn_ctx, n_ouputs, outputs);
}
