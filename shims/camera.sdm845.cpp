//
// SPDX-FileCopyrightText: The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
//

#define LOG_TAG "camera.sdm845.shim"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/properties.h>
#include <android-base/strings.h>
#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/camera_metadata.h>

#define REAL_CAMERA_MODULE_PATH "/vendor/lib/hw/camera.sdm845.orig.so"

static void* g_real_handle = nullptr;
static camera_module_t* g_real_module = nullptr;
static int (*g_real_open)(const struct hw_module_t* module, const char* id,
                          struct hw_device_t** device) = nullptr;
static int (*g_real_get_camera_info)(int camera_id, struct camera_info* info) = nullptr;

static std::mutex g_lock;
static std::map<int, camera_metadata_t*> g_fixed_metadata_cache;

static void ensure_real_module_loaded() {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_real_module != nullptr) {
        return;
    }

    g_real_handle = dlopen(REAL_CAMERA_MODULE_PATH, RTLD_NOW);
    if (!g_real_handle) {
        ALOGE("%s: Failed to dlopen %s: %s", __FUNCTION__, REAL_CAMERA_MODULE_PATH, dlerror());
        return;
    }

    g_real_module = reinterpret_cast<camera_module_t*>(dlsym(g_real_handle, HAL_MODULE_INFO_SYM_AS_STR));
    if (!g_real_module) {
        ALOGE("%s: Failed to find %s in %s", __FUNCTION__, HAL_MODULE_INFO_SYM_AS_STR, REAL_CAMERA_MODULE_PATH);
        dlclose(g_real_handle);
        g_real_handle = nullptr;
        return;
    }

    g_real_get_camera_info = g_real_module->get_camera_info;
    if (g_real_module->common.methods != nullptr) {
        g_real_open = g_real_module->common.methods->open;
    }
}

static int shim_get_camera_info(int camera_id, struct camera_info* info) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_get_camera_info) {
        return -ENODEV;
    }

    int rc = g_real_get_camera_info(camera_id, info);
    if (rc != 0 || info == nullptr || info->static_camera_characteristics == nullptr) {
        return rc;
    }

    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_fixed_metadata_cache.find(camera_id);
    if (it != g_fixed_metadata_cache.end()) {
        info->static_camera_characteristics = it->second;
        return rc;
    }

    const camera_metadata_t* raw = info->static_camera_characteristics;
    camera_metadata_ro_entry_t entry;
    int entry_rc = find_camera_metadata_ro_entry(raw, ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS, &entry);
    if (entry_rc != 0 || entry.count == 0) {
        camera_metadata_ro_entry_t capEntry;
        if (find_camera_metadata_ro_entry(raw, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, &capEntry) == 0) {
            bool isLogical = false;
            for (size_t i = 0; i < capEntry.count; i++) {
                if (capEntry.data.u8[i] == ANDROID_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA) {
                    isLogical = true;
                    break;
                }
            }
            if (isLogical) {
                std::string physicalIdsProp = android::base::GetProperty("ro.vendor.camera.logical.physical_ids", "0,2");
                std::vector<uint8_t> idsList;
                std::vector<std::string> idTokens = android::base::Split(physicalIdsProp, ",");
                for (const auto& token : idTokens) {
                    std::string trimmed = android::base::Trim(token);
                    if (!trimmed.empty()) {
                        for (char c : trimmed) {
                            idsList.push_back(static_cast<uint8_t>(c));
                        }
                        idsList.push_back('\0');
                    }
                }
                if (idsList.empty()) {
                    idsList = {'0', '\0', '2', '\0'};
                }

                size_t entry_count = get_camera_metadata_entry_count(raw) + 1;
                size_t data_count = get_camera_metadata_data_count(raw) + idsList.size() + 16;
                camera_metadata_t* meta = allocate_camera_metadata(entry_count, data_count);
                if (meta != nullptr) {
                    append_camera_metadata(meta, raw);
                    if (entry_rc == 0) {
                        update_camera_metadata_entry(meta, entry.index, idsList.data(), idsList.size(), nullptr);
                    } else {
                        add_camera_metadata_entry(meta, ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS,
                                                  idsList.data(), idsList.size());
                    }
                    g_fixed_metadata_cache[camera_id] = meta;
                    info->static_camera_characteristics = meta;
                    ALOGI("%s: Injected ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS for camera %d",
                          __FUNCTION__, camera_id);
                }
            }
        }
    }

    return rc;
}

static int shim_open(const struct hw_module_t* /*module*/, const char* id, struct hw_device_t** device) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_open) {
        return -ENODEV;
    }
    return g_real_open(reinterpret_cast<const struct hw_module_t*>(g_real_module), id, device);
}

static int shim_get_number_of_cameras(void) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_module->get_number_of_cameras) {
        return 0;
    }
    return g_real_module->get_number_of_cameras();
}

static int shim_set_callbacks(const camera_module_callbacks_t *callbacks) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_module->set_callbacks) {
        return -ENODEV;
    }
    return g_real_module->set_callbacks(callbacks);
}

static void shim_get_vendor_tag_ops(vendor_tag_ops_t* ops) {
    ensure_real_module_loaded();
    if (g_real_module && g_real_module->get_vendor_tag_ops) {
        g_real_module->get_vendor_tag_ops(ops);
    }
}

static int shim_open_legacy(const struct hw_module_t* /*module*/, const char* id,
                            uint32_t halVersion, struct hw_device_t** device) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_module->open_legacy) {
        return -ENOSYS;
    }
    return g_real_module->open_legacy(reinterpret_cast<const struct hw_module_t*>(g_real_module),
                                      id, halVersion, device);
}

static int shim_set_torch_mode(const char* camera_id, bool enabled) {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_module->set_torch_mode) {
        return -ENOSYS;
    }
    return g_real_module->set_torch_mode(camera_id, enabled);
}

static int shim_init() {
    ensure_real_module_loaded();
    if (!g_real_module || !g_real_module->init) {
        return 0;
    }
    return g_real_module->init();
}

static hw_module_methods_t g_shim_module_methods = {
    .open = shim_open,
};

extern "C" {

camera_module_t HAL_MODULE_INFO_SYM __attribute__((visibility("default"))) = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "SDM845 Camera Shim Module",
        .author = "The LineageOS Project",
        .methods = &g_shim_module_methods,
        .dso = nullptr,
        .reserved = {0},
    },
    .get_number_of_cameras = shim_get_number_of_cameras,
    .get_camera_info = shim_get_camera_info,
    .set_callbacks = shim_set_callbacks,
    .get_vendor_tag_ops = shim_get_vendor_tag_ops,
    .open_legacy = shim_open_legacy,
    .set_torch_mode = shim_set_torch_mode,
    .init = shim_init,
    .reserved = {nullptr},
};

void* ChiEntry(void* p1, void* p2) {
    ensure_real_module_loaded();
    typedef void* (*chi_entry_fn)(void*, void*);
    static chi_entry_fn real_fn = g_real_handle ? reinterpret_cast<chi_entry_fn>(dlsym(g_real_handle, "ChiEntry")) : nullptr;
    return real_fn ? real_fn(p1, p2) : nullptr;
}

void* CamxMemAlloc(void* p1) {
    ensure_real_module_loaded();
    typedef void* (*fn_t)(void*);
    static fn_t real_fn = g_real_handle ? reinterpret_cast<fn_t>(dlsym(g_real_handle, "CamxMemAlloc")) : nullptr;
    return real_fn ? real_fn(p1) : nullptr;
}

int CamxMemGetImageSize(void* p1, void* p2) {
    ensure_real_module_loaded();
    typedef int (*fn_t)(void*, void*);
    static fn_t real_fn = g_real_handle ? reinterpret_cast<fn_t>(dlsym(g_real_handle, "CamxMemGetImageSize")) : nullptr;
    return real_fn ? real_fn(p1, p2) : 0;
}

int CamxMemRelease(void* p1) {
    ensure_real_module_loaded();
    typedef int (*fn_t)(void*);
    static fn_t real_fn = g_real_handle ? reinterpret_cast<fn_t>(dlsym(g_real_handle, "CamxMemRelease")) : nullptr;
    return real_fn ? real_fn(p1) : 0;
}

__attribute__((constructor))
static void shim_init_constructor() {
    ensure_real_module_loaded();
}

} // extern "C"

