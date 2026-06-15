#define _GNU_SOURCE

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

typedef void *VADisplay;
typedef unsigned int VABufferID;
typedef int VAStatus;

#define VA_STATUS_ERROR_OPERATION_FAILED 0x00000001
#define VA_STATUS_ERROR_FLAG_NOT_SUPPORTED 0x00000011
#define VA_MAPBUFFER_FLAG_READ 1u
#define VA_MAPBUFFER_FLAG_WRITE 2u

typedef VAStatus (*va_map_buffer_fn)(VADisplay dpy, VABufferID buf_id, void **pbuf);

VAStatus vaMapBuffer2(VADisplay dpy, VABufferID buf_id, void **pbuf, uint32_t flags) {
    static va_map_buffer_fn real_va_map_buffer = NULL;

    if ((flags & ~(VA_MAPBUFFER_FLAG_READ | VA_MAPBUFFER_FLAG_WRITE)) != 0u) {
        return VA_STATUS_ERROR_FLAG_NOT_SUPPORTED;
    }

    if (real_va_map_buffer == NULL) {
        void *handle = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
        if (handle == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        real_va_map_buffer = (va_map_buffer_fn)dlsym(handle, "vaMapBuffer");
        if (real_va_map_buffer == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    return real_va_map_buffer(dpy, buf_id, pbuf);
}
