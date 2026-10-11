// SPDX-License-Identifier: MIT
// Real renderer implementation; Vulkan entry points are a deterministic ICD fake.
#include <stdlib.h>
#include <string.h>

#include <unity.h>

static void *poison_malloc(size_t n) {
    void *p = malloc(n);
    if (p)
        memset(p, 0xa5, n);
    return p;
}
#define malloc poison_malloc
#include "../../src/vk_renderer.c"
#undef malloc
#define HANDLE(type, value) ((type) (uintptr_t) (value))

static struct vk_renderer *renderer;
static int instance_live, device_live, pool_live, result_calls, fail_call;
static uint32_t api_version;
static bool memory_bit31;
static bool no_device_extensions;
static VkResult result(void) {
    return ++result_calls == fail_call ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}
static const char *extensions[] = { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                                    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,   VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
                                    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, NULL };
VkResult vkEnumerateInstanceLayerProperties(uint32_t *n, VkLayerProperties *p) {
    if (p)
        strcpy(p[0].layerName, "test-layer");
    *n = 1;
    return result();
}
VkResult vkEnumerateInstanceExtensionProperties(const char *name, uint32_t *n, VkExtensionProperties *p) {
    (void) name;
    if (p)
        strcpy(p[0].extensionName, "test-extension");
    *n = 1;
    return result();
}
VkResult vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void) ci;
    (void) a;
    VkResult r = result();
    if (!r) {
        *out = HANDLE(VkInstance, 1);
        instance_live++;
    }
    return r;
}
void vkDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) {
    (void) i;
    (void) a;
    instance_live--;
}
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance i, const char *name) {
    (void) i;
    (void) name;
    return NULL;
}
VkResult vkEnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    (void) i;
    *n = 1;
    if (p)
        p[0] = HANDLE(VkPhysicalDevice, 2);
    return result();
}
void vkGetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) {
    (void) d;
    memset(p, 0, sizeof(*p));
    p->apiVersion = api_version;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}
void vkGetPhysicalDeviceFeatures(VkPhysicalDevice d, VkPhysicalDeviceFeatures *f) {
    (void) d;
    memset(f, 0, sizeof(*f));
}
void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t *n, VkQueueFamilyProperties *p) {
    (void) d;
    *n = 1;
    if (p)
        p[0] = (VkQueueFamilyProperties) { .queueFlags = VK_QUEUE_GRAPHICS_BIT, .queueCount = 1 };
}
VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char *l, uint32_t *n, VkExtensionProperties *p) {
    (void) d;
    (void) l;
    *n = no_device_extensions ? 0 : ARRAY_SIZE(extensions) - 1;
    if (p)
        for (unsigned i = 0; i < *n; i++)
            strcpy(p[i].extensionName, extensions[i]);
    return result();
}
VkResult vkCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void) d;
    (void) ci;
    (void) a;
    VkResult r = result();
    if (!r) {
        *out = HANDLE(VkDevice, 3);
        device_live++;
    }
    return r;
}
void vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *a) {
    (void) d;
    (void) a;
    device_live--;
}
void vkGetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue *out) {
    (void) d;
    (void) family;
    (void) index;
    *out = HANDLE(VkQueue, 4);
}
VkResult vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void) d;
    (void) ci;
    (void) a;
    VkResult r = result();
    if (!r) {
        *out = HANDLE(VkCommandPool, 5);
        pool_live++;
    }
    return r;
}
void vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) {
    (void) d;
    (void) p;
    (void) a;
    pool_live--;
}
VkResult vkDeviceWaitIdle(VkDevice d) {
    (void) d;
    return VK_SUCCESS;
}
static VkResult fake_submit(VkQueue q, uint32_t n, const VkSubmitInfo *infos, VkFence f) {
    (void) q;
    (void) n;
    (void) infos;
    (void) f;
    return VK_SUCCESS;
}
static VkResult fake_queue_wait(VkQueue q) {
    (void) q;
    return VK_SUCCESS;
}
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice d, const char *name) {
    (void) d;
    if (!strcmp(name, "vkQueueSubmit"))
        return (PFN_vkVoidFunction) fake_submit;
    if (!strcmp(name, "vkQueueWaitIdle"))
        return (PFN_vkVoidFunction) fake_queue_wait;
    if (!strcmp(name, "vkDeviceWaitIdle"))
        return (PFN_vkVoidFunction) vkDeviceWaitIdle;
    return NULL;
}
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p) {
    (void) d;
    memset(p, 0, sizeof(*p));
    p->memoryTypeCount = memory_bit31 ? 32 : 1;
    p->memoryHeapCount = 1;
    p->memoryHeaps[0].size = 1024 * 1024;
    p->memoryTypes[memory_bit31 ? 31 : 0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
}
void vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice d,
    const VkPhysicalDeviceExternalSemaphoreInfo *i,
    VkExternalSemaphoreProperties *p
) {
    (void) d;
    (void) i;
    p->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
}
void setUp(void) {
    renderer = NULL;
    instance_live = device_live = pool_live = result_calls = fail_call = 0;
    api_version = VK_MAKE_VERSION(1, 1, 0);
    memory_bit31 = false;
    no_device_extensions = false;
}
void tearDown(void) {
    // Direct destruction deliberately bypasses the broken constructor refcount.
    if (renderer)
        vk_renderer_destroy(renderer);
    TEST_ASSERT_EQUAL_INT(0, instance_live);
    TEST_ASSERT_EQUAL_INT(0, device_live);
    TEST_ASSERT_EQUAL_INT(0, pool_live);
}
static void constructor_initializes_reference_count(void) {
    renderer = vk_renderer_new();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&renderer->n_refs));
}
static void validation_callback_does_not_abort_vulkan_calls(void) {
    VkDebugUtilsMessengerCallbackDataEXT data = { .pMessageIdName = "audit", .pMessage = "diagnostic" };
    TEST_ASSERT_EQUAL_UINT32(
        VK_FALSE,
        on_debug_utils_message(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data, NULL)
    );
}
static void rejects_device_below_required_api_version(void) {
    api_version = VK_MAKE_VERSION(1, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, score_physical_device(HANDLE(VkPhysicalDevice, 2), extensions));
}
static void rejects_device_with_no_extensions_without_undefined_behavior(void) {
    // A core-only device is unsuitable, not an invalid enumeration result.
    // UBSan detects the production zero-bound VLA before rejection can occur.
    no_device_extensions = true;
    TEST_ASSERT_EQUAL_INT(0, score_physical_device(HANDLE(VkPhysicalDevice, 2), extensions));
}
static void accepts_device_at_required_api_version(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, score_physical_device(HANDLE(VkPhysicalDevice, 2), extensions));
}
static void constructor_failure_unwinds_created_handles(void) {
    renderer = vk_renderer_new();
    TEST_ASSERT_NOT_NULL(renderer);
    int n = result_calls;
    vk_renderer_destroy(renderer);
    renderer = NULL;
    for (int i = 1; i <= n; i++) {
        result_calls = 0;
        fail_call = i;
        renderer = vk_renderer_new();
        TEST_ASSERT_NULL(renderer);
        TEST_ASSERT_EQUAL_INT(0, instance_live);
        TEST_ASSERT_EQUAL_INT(0, device_live);
        TEST_ASSERT_EQUAL_INT(0, pool_live);
    }
}
static void memory_type_bit31_has_no_undefined_shift(void) {
    // Normal builds pass; UBSan must reject the signed 1 << 31 in production.
    memory_bit31 = true;
    struct vk_renderer fake = { 0 };
    TEST_ASSERT_EQUAL_INT(31, vk_renderer_find_mem_type(&fake, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, UINT32_C(1) << 31));
}
int main(int argc, char **argv) {
    UNITY_BEGIN();
    int selected = 0;
#define CASE(f)                                  \
    if (argc == 1 || strcmp(argv[1], #f) == 0) { \
        selected++;                              \
        RUN_TEST(f);                             \
    }
    CASE(constructor_initializes_reference_count)
    CASE(validation_callback_does_not_abort_vulkan_calls)
    CASE(rejects_device_below_required_api_version)
    CASE(rejects_device_with_no_extensions_without_undefined_behavior)
    CASE(accepts_device_at_required_api_version)
    CASE(constructor_failure_unwinds_created_handles)
    CASE(memory_type_bit31_has_no_undefined_shift)
    return selected ? UNITY_END() : 2;
}
