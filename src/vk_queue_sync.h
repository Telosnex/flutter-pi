// SPDX-License-Identifier: MIT
#ifndef FLUTTERPI_VK_QUEUE_SYNC_H
#define FLUTTERPI_VK_QUEUE_SYNC_H

#include <vulkan/vulkan.h>

// Register every queue handed to the engine before it starts. Registration owns
// no Vulkan handles. Unregister only after engine shutdown and surface teardown.
int vk_queue_sync_register(VkDevice device, VkQueue queue);
void vk_queue_sync_unregister(VkDevice device);

// Resolver to supply to Flutter. Nested instance/device lookups stay wrapped.
PFN_vkVoidFunction VKAPI_CALL vk_queue_sync_get_instance_proc_addr(VkInstance instance, const char *name);

// The runner must use these same entry points, never raw queue/idle calls.
VKAPI_ATTR VkResult VKAPI_CALL vk_queue_sync_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vk_queue_sync_wait_idle(VkQueue queue);
VKAPI_ATTR VkResult VKAPI_CALL vk_queue_sync_device_wait_idle(VkDevice device);

#endif
