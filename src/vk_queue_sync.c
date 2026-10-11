// SPDX-License-Identifier: MIT
#include "vk_queue_sync.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

// Process-lifetime lock, deliberately shared by ALL registered queues/devices:
// vkDeviceWaitIdle must exclude access to every queue of its device. The runner
// currently creates one device/queue. Over-serializing separate devices avoids
// lock-order and lock-lifetime hazards; this lock is never destroyed/replaced.
// Engine QueueVK mutex -> this mutex -> driver. Never call into Flutter here.
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

struct queue_dispatch {
    struct queue_dispatch *next;
    VkDevice device;
    VkQueue queue;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueWaitIdle wait_idle;
    PFN_vkDeviceWaitIdle device_wait_idle;
    PFN_vkQueueBindSparse bind_sparse;
    PFN_vkQueuePresentKHR present;
    PFN_vkQueueBeginDebugUtilsLabelEXT begin_label;
    PFN_vkQueueEndDebugUtilsLabelEXT end_label;
    PFN_vkQueueInsertDebugUtilsLabelEXT insert_label;
#ifdef VK_VERSION_1_3
    PFN_vkQueueSubmit2 submit2;
#endif
#ifdef VK_KHR_synchronization2
    PFN_vkQueueSubmit2KHR submit2_khr;
#endif
};
static struct queue_dispatch *queues;

// Caller holds host_lock. Driver pointers always come from the REAL resolver,
// not the intercepted one. No renderer pointers or engine-owned state are kept.
static struct queue_dispatch *find_queue(VkQueue queue) {
    for (struct queue_dispatch *p = queues; p; p = p->next)
        if (p->queue == queue)
            return p;
    return NULL;
}
static struct queue_dispatch *find_device(VkDevice device) {
    for (struct queue_dispatch *p = queues; p; p = p->next)
        if (p->device == device)
            return p;
    return NULL;
}

int vk_queue_sync_register(VkDevice device, VkQueue queue) {
    if (!device || !queue)
        return EINVAL;
    struct queue_dispatch *p = calloc(1, sizeof(*p));
    if (!p)
        return ENOMEM;
    p->device = device;
    p->queue = queue;
#define LOAD(field, name) p->field = (PFN_##name) vkGetDeviceProcAddr(device, #name)
    LOAD(submit, vkQueueSubmit);
    LOAD(wait_idle, vkQueueWaitIdle);
    LOAD(device_wait_idle, vkDeviceWaitIdle);
    LOAD(bind_sparse, vkQueueBindSparse);
    LOAD(present, vkQueuePresentKHR);
    LOAD(begin_label, vkQueueBeginDebugUtilsLabelEXT);
    LOAD(end_label, vkQueueEndDebugUtilsLabelEXT);
    LOAD(insert_label, vkQueueInsertDebugUtilsLabelEXT);
#ifdef VK_VERSION_1_3
    LOAD(submit2, vkQueueSubmit2);
#endif
#ifdef VK_KHR_synchronization2
    LOAD(submit2_khr, vkQueueSubmit2KHR);
#endif
#undef LOAD
    if (!p->submit || !p->wait_idle || !p->device_wait_idle) {
        free(p);
        return ENOTSUP;
    }
    pthread_mutex_lock(&host_lock);
    if (find_queue(queue)) {
        pthread_mutex_unlock(&host_lock);
        free(p);
        return EEXIST;
    }
    p->next = queues;
    queues = p;
    pthread_mutex_unlock(&host_lock);
    return 0;
}

void vk_queue_sync_unregister(VkDevice device) {
    pthread_mutex_lock(&host_lock);
    struct queue_dispatch **link = &queues;
    while (*link) {
        struct queue_dispatch *p = *link;
        if (p->device == device) {
            *link = p->next;
            free(p);
        } else
            link = &p->next;
    }
    pthread_mutex_unlock(&host_lock);
}

// Keeping the lock across the actual driver call is essential. Merely protecting
// lookup, or taking a runner-only surface mutex, would not synchronize the queue.
#define RESULT_WRAPPER(name, field, lookup, params, args)                                                              \
    VKAPI_ATTR VkResult VKAPI_CALL name params {                                                                       \
        pthread_mutex_lock(&host_lock);                                                                                \
        struct queue_dispatch *p = lookup;                                                                             \
        VkResult result = p && p->field ? p->field args : (p ? VK_ERROR_EXTENSION_NOT_PRESENT : VK_ERROR_DEVICE_LOST); \
        pthread_mutex_unlock(&host_lock);                                                                              \
        return result;                                                                                                 \
    }
RESULT_WRAPPER(
    vk_queue_sync_submit,
    submit,
    find_queue(queue),
    (VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence),
    (queue, count, submits, fence)
)
RESULT_WRAPPER(vk_queue_sync_wait_idle, wait_idle, find_queue(queue), (VkQueue queue), (queue))
RESULT_WRAPPER(vk_queue_sync_device_wait_idle, device_wait_idle, find_device(device), (VkDevice device), (device))
static RESULT_WRAPPER(
    queue_bind_sparse,
    bind_sparse,
    find_queue(queue),
    (VkQueue queue, uint32_t count, const VkBindSparseInfo *binds, VkFence fence),
    (queue, count, binds, fence)
) static RESULT_WRAPPER(queue_present, present, find_queue(queue), (VkQueue queue, const VkPresentInfoKHR *info), (queue, info))
#ifdef VK_VERSION_1_3
    static RESULT_WRAPPER(
        queue_submit2,
        submit2,
        find_queue(queue),
        (VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence),
        (queue, count, submits, fence)
    )
#endif
#ifdef VK_KHR_synchronization2
        static RESULT_WRAPPER(
            queue_submit2_khr,
            submit2_khr,
            find_queue(queue),
            (VkQueue queue, uint32_t count, const VkSubmitInfo2KHR *submits, VkFence fence),
            (queue, count, submits, fence)
        )
#endif
#undef RESULT_WRAPPER

#define VOID_WRAPPER(name, field, params, args)       \
    static VKAPI_ATTR void VKAPI_CALL name params {   \
        pthread_mutex_lock(&host_lock);               \
        struct queue_dispatch *p = find_queue(queue); \
        if (p && p->field)                            \
            p->field args;                            \
        pthread_mutex_unlock(&host_lock);             \
    }
            VOID_WRAPPER(queue_begin_label, begin_label, (VkQueue queue, const VkDebugUtilsLabelEXT *label), (queue, label))
                VOID_WRAPPER(queue_end_label, end_label, (VkQueue queue), (queue))
                    VOID_WRAPPER(queue_insert_label, insert_label, (VkQueue queue, const VkDebugUtilsLabelEXT *label), (queue, label))
#undef VOID_WRAPPER

                        static PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char *name);

static PFN_vkVoidFunction intercept(const char *name, PFN_vkVoidFunction real) {
    // In particular, do NOT manufacture availability for optional extensions.
    if (!real)
        return NULL;
#define ENTRY(vulkan_name, wrapper)      \
    if (strcmp(name, #vulkan_name) == 0) \
    return (PFN_vkVoidFunction) wrapper
    ENTRY(vkGetInstanceProcAddr, vk_queue_sync_get_instance_proc_addr);
    ENTRY(vkGetDeviceProcAddr, get_device_proc_addr);
    ENTRY(vkQueueSubmit, vk_queue_sync_submit);
    ENTRY(vkQueueWaitIdle, vk_queue_sync_wait_idle);
    ENTRY(vkDeviceWaitIdle, vk_queue_sync_device_wait_idle);
    ENTRY(vkQueueBindSparse, queue_bind_sparse);
    ENTRY(vkQueuePresentKHR, queue_present);
    ENTRY(vkQueueBeginDebugUtilsLabelEXT, queue_begin_label);
    ENTRY(vkQueueEndDebugUtilsLabelEXT, queue_end_label);
    ENTRY(vkQueueInsertDebugUtilsLabelEXT, queue_insert_label);
#ifdef VK_VERSION_1_3
    ENTRY(vkQueueSubmit2, queue_submit2);
#endif
#ifdef VK_KHR_synchronization2
    ENTRY(vkQueueSubmit2KHR, queue_submit2_khr);
#endif
#undef ENTRY
    // Fail closed for future/vendor queue entry points until explicitly wrapped.
    // None of these are enabled/required by this runner's Vulkan configuration.
    if (strncmp(name, "vkQueue", 7) == 0)
        return NULL;
    return real;
}

static PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr(VkDevice device, const char *name) {
    return intercept(name, vkGetDeviceProcAddr(device, name));
}

PFN_vkVoidFunction VKAPI_CALL vk_queue_sync_get_instance_proc_addr(VkInstance instance, const char *name) {
    if (strcmp(name, "GetInstanceProcAddr") == 0)
        name = "vkGetInstanceProcAddr";
    return intercept(name, vkGetInstanceProcAddr(instance, name));
}
