#ifndef NVK_PHYSICAL_DEVICE_H
#define NVK_PHYSICAL_DEVICE_H 1

#include "nvk_private.h"

#include "nouveau_device.h"
#include "nv_device_info.h"

#include "vulkan/runtime/vk_physical_device.h"

#include "wsi_common.h"

struct nvk_instance;

struct nvk_physical_device {
   struct vk_physical_device vk;
   struct nvk_instance *instance;
   struct nouveau_ws_device *dev;
   struct nv_device_info info;
   struct wsi_device wsi_device;

   // TODO: add mapable VRAM heap if possible
   VkMemoryHeap mem_heaps[2];
   VkMemoryType mem_types[2];
   uint8_t mem_heap_cnt;
   uint8_t mem_type_cnt;

   const struct vk_sync_type *sync_types[2];
};

VK_DEFINE_HANDLE_CASTS(nvk_physical_device,
   vk.base,
   VkPhysicalDevice,
   VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkResult nvk_create_drm_physical_device(struct vk_instance *vk_instance,
                                        struct _drmDevice *device,
                                        struct vk_physical_device **out);

void nvk_physical_device_destroy(struct vk_physical_device *vk_device);

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
    defined(VK_USE_PLATFORM_XCB_KHR) || \
    defined(VK_USE_PLATFORM_XLIB_KHR) || \
    defined(VK_USE_PLATFORM_DISPLAY_KHR)
#define NVK_USE_WSI_PLATFORM
#endif

#endif