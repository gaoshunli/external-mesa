#include "nvk_device_memory.h"

#include "nouveau_bo.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nv_push.h"

#include <inttypes.h>
#include <sys/mman.h>

#include "nvtypes.h"
#include "nvk_cl902d.h"

static VkResult
zero_vram(struct nvk_device *dev, struct nouveau_ws_bo *bo)
{
   uint32_t push_data[256];
   struct nv_push push;
   nv_push_init(&push, push_data, ARRAY_SIZE(push_data));
   struct nv_push *p = &push;

   uint64_t addr = bo->offset;

   /* can't go higher for whatever reason */
   uint32_t pitch = 1 << 19;

   P_IMMD(p, NV902D, SET_OPERATION, V_SRCCOPY);

   P_MTHD(p, NV902D, SET_DST_FORMAT);
   P_NV902D_SET_DST_FORMAT(p, V_A8B8G8R8);
   P_NV902D_SET_DST_MEMORY_LAYOUT(p, V_PITCH);

   P_MTHD(p, NV902D, SET_DST_PITCH);
   P_NV902D_SET_DST_PITCH(p, pitch);

   P_MTHD(p, NV902D, SET_DST_OFFSET_UPPER);
   P_NV902D_SET_DST_OFFSET_UPPER(p, addr >> 32);
   P_NV902D_SET_DST_OFFSET_LOWER(p, addr & 0xffffffff);

   P_MTHD(p, NV902D, SET_RENDER_SOLID_PRIM_COLOR_FORMAT);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT(p, V_A8B8G8R8);
   P_NV902D_SET_RENDER_SOLID_PRIM_COLOR(p, 0);

   uint32_t height = bo->size / pitch;
   uint32_t extra = bo->size % pitch;

   if (height > 0) {
      P_IMMD(p, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

      P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 0, 0);
      P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 1, pitch / 4);
      P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 1, height);
   }

   P_IMMD(p, NV902D, RENDER_SOLID_PRIM_MODE, V_RECTS);

   P_MTHD(p, NV902D, RENDER_SOLID_PRIM_POINT_SET_X(0));
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 0, 0);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 0, height);
   P_NV902D_RENDER_SOLID_PRIM_POINT_SET_X(p, 1, extra / 4);
   P_NV902D_RENDER_SOLID_PRIM_POINT_Y(p, 1, height);

   return nvk_queue_submit_simple(&dev->queue, nv_push_dw_count(&push),
                                  push_data, 1, &bo, false /* sync */);
}

VkResult
nvk_allocate_memory(struct nvk_device *device,
                    const VkMemoryAllocateInfo *pAllocateInfo,
                    const struct nvk_memory_tiling_info *tile_info,
                    const VkAllocationCallbacks *pAllocator,
                    struct nvk_device_memory **mem_out)
{
   VkMemoryType *type = &device->pdev->mem_types[pAllocateInfo->memoryTypeIndex];
   struct nvk_device_memory *mem;

   mem = vk_object_alloc(&device->vk, pAllocator, sizeof(*mem),
                         VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (!mem)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   enum nouveau_ws_bo_flags flags;
   if (type->propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
      flags = NOUVEAU_WS_BO_LOCAL;
   else
      flags = NOUVEAU_WS_BO_GART;

   if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      flags |= NOUVEAU_WS_BO_MAP;

   mem->map = NULL;
   if (tile_info) {
      mem->bo = nouveau_ws_bo_new_tiled(device->pdev->dev,
                                        pAllocateInfo->allocationSize, 0,
                                        tile_info->pte_kind,
                                        tile_info->tile_mode,
                                        flags);
   } else {
      mem->bo = nouveau_ws_bo_new(device->pdev->dev,
                                  pAllocateInfo->allocationSize, 0, flags);
   }
   if (!mem->bo) {
      vk_object_free(&device->vk, pAllocator, mem);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   VkResult result;
   if (device->pdev->dev->debug_flags & NVK_DEBUG_ZERO_MEMORY) {
      if (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
         void *map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
         if (map == NULL) {
            result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                               "Memory map failed");
            goto fail_bo;
         }
         memset(map, 0, mem->bo->size);
         nouveau_ws_bo_unmap(mem->bo, map);
         result = VK_SUCCESS;
      } else {
         result = zero_vram(device, mem->bo);
         if (result != VK_SUCCESS)
            goto fail_bo;
      }
   }

   simple_mtx_lock(&device->memory_objects_lock);
   list_addtail(&mem->link, &device->memory_objects);
   simple_mtx_unlock(&device->memory_objects_lock);

   *mem_out = mem;

   return VK_SUCCESS;

fail_bo:
   nouveau_ws_bo_destroy(mem->bo);
   vk_object_free(&device->vk, pAllocator, mem);
   return result;
}

void
nvk_free_memory(struct nvk_device *device,
                struct nvk_device_memory *mem,
                const VkAllocationCallbacks *pAllocator)
{
   if (mem->map)
      nouveau_ws_bo_unmap(mem->bo, mem->map);

   simple_mtx_lock(&device->memory_objects_lock);
   list_del(&mem->link);
   simple_mtx_unlock(&device->memory_objects_lock);

   nouveau_ws_bo_destroy(mem->bo);

   vk_object_free(&device->vk, pAllocator, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateMemory(VkDevice _device,
                   const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_device_memory *mem;
   VkResult result;

   result = nvk_allocate_memory(device, pAllocateInfo, NULL, pAllocator, &mem);
   if (result != VK_SUCCESS)
      return result;

   *pMem = nvk_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeMemory(VkDevice _device,
               VkDeviceMemory _mem,
               const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   if (!mem)
      return;

   nvk_free_memory(device, mem, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_MapMemory(VkDevice _device,
              VkDeviceMemory _memory,
              VkDeviceSize offset,
              VkDeviceSize size,
              VkMemoryMapFlags flags,
              void **ppData)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (size == VK_WHOLE_SIZE)
      size = mem->bo->size - offset;

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%"PRIx64" does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->map != NULL) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");
   }

   mem->map = nouveau_ws_bo_map(mem->bo, NOUVEAU_WS_BO_RDWR);
   if (mem->map == NULL) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object couldn't be mapped.");
   }

   *ppData = mem->map + offset;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_UnmapMemory(VkDevice _device,
                VkDeviceMemory _memory)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, _memory);

   if (mem == NULL)
      return;

   nouveau_ws_bo_unmap(mem->bo, mem->map);
   mem->map = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FlushMappedMemoryRanges(VkDevice _device,
                            uint32_t memoryRangeCount,
                            const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_InvalidateMappedMemoryRanges(VkDevice _device,
                                 uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDeviceMemoryCommitment(VkDevice device,
                              VkDeviceMemory _mem,
                              VkDeviceSize* pCommittedMemoryInBytes)
{
   VK_FROM_HANDLE(nvk_device_memory, mem, _mem);

   *pCommittedMemoryInBytes = mem->bo->size;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetMemoryFdKHR(VkDevice _device,
                   const VkMemoryGetFdInfoKHR *pGetFdInfo,
                   int *pFD)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_device_memory, memory, pGetFdInfo->memory);

   switch (pGetFdInfo->handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      if (nouveau_ws_bo_dma_buf(memory->bo, pFD))
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return VK_SUCCESS;
   default:
      assert(!"unsupported handle type");
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
   }
}