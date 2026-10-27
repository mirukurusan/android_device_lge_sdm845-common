/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <utils/Errors.h>
#include <utils/String8.h>

namespace fake_utils {
class String8Shim {
public:
    const char* mString;

    android::String8 getPathLeaf() const asm("_ZNK7android7String811getPathLeafEv");
};

android::String8 String8Shim::getPathLeaf() const {
    const char* cp = strrchr(mString, '/');
    if (cp == nullptr) {
        return android::String8(mString);
    } else {
        return android::String8(cp + 1);
    }
}
}  // namespace fake_utils

namespace fake_ui {
class GraphicBufferMapperShim {
public:
    android::status_t lock(buffer_handle_t handle, uint32_t usage, const android::Rect& bounds,
                           void** vaddr, int32_t* outBytesPerPixel, int32_t* outBytesPerStride)
            asm("_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_");

    android::status_t unlock(buffer_handle_t handle)
            asm("_ZN7android19GraphicBufferMapper6unlockEPK13native_handle");
};

android::status_t GraphicBufferMapperShim::lock(buffer_handle_t handle, uint32_t usage,
                                               const android::Rect& bounds, void** vaddr,
                                               int32_t* outBytesPerPixel,
                                               int32_t* outBytesPerStride) {
    auto* self = reinterpret_cast<android::GraphicBufferMapper*>(this);
    auto result = self->lock(handle, static_cast<int64_t>(usage), bounds);
    if (!result.has_value()) return result.asStatus();
    auto val = result.value();
    *vaddr = val.address;
    if (outBytesPerPixel) *outBytesPerPixel = val.bytesPerPixel;
    if (outBytesPerStride) *outBytesPerStride = val.bytesPerStride;
    return android::OK;
}

android::status_t GraphicBufferMapperShim::unlock(buffer_handle_t handle) {
    auto* self = reinterpret_cast<android::GraphicBufferMapper*>(this);
    return self->unlock(handle);
}
}  // namespace fake_ui
