// SPDX-License-Identifier: MIT
// Real dispatch implementation; only the Vulkan driver and lock-attempt
// observation are fake. No sleeps or GPU are needed for interleaving tests.
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <unity.h>

static int observed_lock(pthread_mutex_t *mutex);
static void *test_calloc(size_t count, size_t size);
#define calloc test_calloc
#define pthread_mutex_lock observed_lock
#include "../../src/vk_queue_sync.c"
#undef pthread_mutex_lock
#undef calloc

#define HANDLE(type, n) ((type) (uintptr_t) (n))
#define DEVICE HANDLE(VkDevice, 1)
#define QUEUE HANDLE(VkQueue, 2)
#define QUEUE2 HANDLE(VkQueue, 3)
#define INSTANCE HANDLE(VkInstance, 4)
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local bool contender;
static bool attempt, hold_first, release_first, optional_available;
static int entered, active, max_active, unlocked_entries;
static VkResult driver_result;
static bool fail_registration_alloc;
static const char *missing_entry;
static VkQueue submitted_queue;
static uint32_t submitted_count;
static const VkSubmitInfo *submitted_infos;
static VkFence submitted_fence;
static void *test_calloc(size_t count, size_t size) {
    return fail_registration_alloc ? NULL : calloc(count, size);
}

static int observed_lock(pthread_mutex_t *mutex) {
    if (mutex == &host_lock && contender) {
        pthread_mutex_lock(&gate);
        attempt = true;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&gate);
    }
    return pthread_mutex_lock(mutex);
}
static VkResult driver_entry(void) {
    // This runs *inside* the fake driver, not merely at wrapper entry.
    int locked = pthread_mutex_trylock(&host_lock);
    if (locked == 0)
        pthread_mutex_unlock(&host_lock);
    pthread_mutex_lock(&gate);
    if (locked != EBUSY)
        unlocked_entries++;
    entered++;
    active++;
    if (active > max_active)
        max_active = active;
    pthread_cond_broadcast(&changed);
    while (hold_first && entered == 1 && !release_first)
        pthread_cond_wait(&changed, &gate);
    active--;
    VkResult result = driver_result;
    pthread_mutex_unlock(&gate);
    return result;
}
static VkResult VKAPI_CALL fake_submit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    submitted_queue = q;
    submitted_count = n;
    submitted_infos = s;
    submitted_fence = f;
    return driver_entry();
}
static VkResult VKAPI_CALL fake_queue_wait(VkQueue q) {
    (void) q;
    return driver_entry();
}
static VkResult VKAPI_CALL fake_device_wait(VkDevice d) {
    (void) d;
    return driver_entry();
}
static VkResult VKAPI_CALL fake_sparse(VkQueue q, uint32_t n, const VkBindSparseInfo *s, VkFence f) {
    (void) q;
    (void) n;
    (void) s;
    (void) f;
    return driver_entry();
}
static VkResult VKAPI_CALL fake_present(VkQueue q, const VkPresentInfoKHR *info) {
    (void) q;
    (void) info;
    return driver_entry();
}
static void VKAPI_CALL fake_label(VkQueue q, const VkDebugUtilsLabelEXT *label) {
    (void) q;
    (void) label;
    driver_entry();
}
static void VKAPI_CALL fake_end_label(VkQueue q) {
    (void) q;
    driver_entry();
}
static VkResult VKAPI_CALL fake_submit2(VkQueue q, uint32_t n, const VkSubmitInfo2KHR *s, VkFence f) {
    (void) q;
    (void) n;
    (void) s;
    (void) f;
    return driver_entry();
}
static void VKAPI_CALL unrelated(void) {
}
static PFN_vkVoidFunction real_lookup(const char *name) {
    if (missing_entry && !strcmp(name, missing_entry))
        return NULL;
#define MAP(n, f)         \
    if (!strcmp(name, n)) \
    return (PFN_vkVoidFunction) f
    MAP("vkGetInstanceProcAddr", vkGetInstanceProcAddr);
    MAP("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
    MAP("vkQueueSubmit", fake_submit);
    MAP("vkQueueWaitIdle", fake_queue_wait);
    MAP("vkDeviceWaitIdle", fake_device_wait);
    MAP("vkQueueBindSparse", fake_sparse);
    MAP("vkUnrelated", unrelated);
    MAP("vkQueueFutureVendorCommand", unrelated);
    if (!optional_available)
        return NULL;
    MAP("vkQueuePresentKHR", fake_present);
    MAP("vkQueueSubmit2", fake_submit2);
    MAP("vkQueueSubmit2KHR", fake_submit2);
    MAP("vkQueueBeginDebugUtilsLabelEXT", fake_label);
    MAP("vkQueueEndDebugUtilsLabelEXT", fake_end_label);
    MAP("vkQueueInsertDebugUtilsLabelEXT", fake_label);
#undef MAP
    return NULL;
}
PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void) instance;
    return real_lookup(name);
}
PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    (void) device;
    return real_lookup(name);
}
static PFN_vkVoidFunction engine_proc(const char *name) {
    // Simulate Flutter/Vulkan-Hpp following both resolver indirections.
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) vk_queue_sync_get_instance_proc_addr(INSTANCE, "GetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr) gipa(INSTANCE, "vkGetDeviceProcAddr");
    return gdpa(DEVICE, name);
}
void setUp(void) {
    fail_registration_alloc = false;
    missing_entry = NULL;
    contender = attempt = hold_first = release_first = false;
    optional_available = true;
    entered = active = max_active = unlocked_entries = 0;
    driver_result = VK_SUCCESS;
    TEST_ASSERT_EQUAL_INT(0, vk_queue_sync_register(DEVICE, QUEUE));
    TEST_ASSERT_EQUAL_INT(0, vk_queue_sync_register(DEVICE, QUEUE2));
}
void tearDown(void) {
    vk_queue_sync_unregister(DEVICE);
}

enum operation { ENGINE_SUBMIT, RUNNER_SUBMIT, ENGINE_IDLE, RUNNER_IDLE, QUEUE_IDLE, SUBMIT2, SPARSE, PRESENT, LABEL, UNREGISTER };
static VkResult call(enum operation operation, VkQueue queue) {
    switch (operation) {
        case ENGINE_SUBMIT: return ((PFN_vkQueueSubmit) engine_proc("vkQueueSubmit"))(queue, 0, NULL, VK_NULL_HANDLE);
        case RUNNER_SUBMIT: return vk_queue_sync_submit(queue, 0, NULL, VK_NULL_HANDLE);
        case ENGINE_IDLE: return ((PFN_vkDeviceWaitIdle) engine_proc("vkDeviceWaitIdle"))(DEVICE);
        case RUNNER_IDLE: return vk_queue_sync_device_wait_idle(DEVICE);
        case QUEUE_IDLE: return ((PFN_vkQueueWaitIdle) engine_proc("vkQueueWaitIdle"))(queue);
        case SUBMIT2: return ((PFN_vkQueueSubmit2KHR) engine_proc("vkQueueSubmit2KHR"))(queue, 0, NULL, VK_NULL_HANDLE);
        case SPARSE: return ((PFN_vkQueueBindSparse) engine_proc("vkQueueBindSparse"))(queue, 0, NULL, VK_NULL_HANDLE);
        case PRESENT: return ((PFN_vkQueuePresentKHR) engine_proc("vkQueuePresentKHR"))(queue, NULL);
        case LABEL: ((PFN_vkQueueInsertDebugUtilsLabelEXT) engine_proc("vkQueueInsertDebugUtilsLabelEXT"))(queue, NULL); return VK_SUCCESS;
        case UNREGISTER: vk_queue_sync_unregister(DEVICE); return VK_SUCCESS;
    }
    return VK_ERROR_UNKNOWN;
}
struct job {
    enum operation operation;
    VkQueue queue;
    bool contend;
    VkResult result;
};
static void *run_job(void *data) {
    struct job *job = data;
    contender = job->contend;
    job->result = call(job->operation, job->queue);
    return NULL;
}
static void prove_exclusion(enum operation first, enum operation second, VkQueue other_queue) {
    hold_first = true;
    struct job a = { first, QUEUE, false, VK_ERROR_UNKNOWN };
    struct job b = { second, other_queue, true, VK_ERROR_UNKNOWN };
    pthread_t ta, tb;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ta, NULL, run_job, &a));
    pthread_mutex_lock(&gate);
    while (entered == 0)
        pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    int create_result = pthread_create(&tb, NULL, run_job, &b);
    pthread_mutex_lock(&gate);
    if (create_result == 0)
        while (!attempt)
            pthread_cond_wait(&changed, &gate);
    int before_release = entered;
    release_first = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    pthread_join(ta, NULL);
    if (create_result == 0)
        pthread_join(tb, NULL);
    // No assertion can longjmp past a held driver call or a live worker.
    TEST_ASSERT_EQUAL_INT(0, create_result);
    TEST_ASSERT_EQUAL_INT(1, before_release);
    TEST_ASSERT_EQUAL_INT(1, max_active);
    TEST_ASSERT_EQUAL_INT(0, unlocked_entries);
    TEST_ASSERT_EQUAL_INT(second == UNREGISTER ? 1 : 2, entered);
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, a.result);
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, b.result);
}
static void engine_submit_excludes_runner_submit(void) {
    prove_exclusion(ENGINE_SUBMIT, RUNNER_SUBMIT, QUEUE);
}
static void engine_upload_excludes_runner_device_idle(void) {
    prove_exclusion(ENGINE_SUBMIT, RUNNER_IDLE, QUEUE);
}
static void engine_device_idle_excludes_runner_submit(void) {
    prove_exclusion(ENGINE_IDLE, RUNNER_SUBMIT, QUEUE);
}
static void runner_device_idle_excludes_engine_upload(void) {
    prove_exclusion(RUNNER_IDLE, ENGINE_SUBMIT, QUEUE);
}
static void shared_lock_covers_all_device_queues(void) {
    prove_exclusion(RUNNER_IDLE, ENGINE_SUBMIT, QUEUE2);
}
static void queue_wait_is_serialized(void) {
    prove_exclusion(ENGINE_SUBMIT, QUEUE_IDLE, QUEUE);
}
static void submit2_alias_is_serialized(void) {
    prove_exclusion(ENGINE_SUBMIT, SUBMIT2, QUEUE);
}
static void sparse_bind_is_serialized(void) {
    prove_exclusion(ENGINE_SUBMIT, SPARSE, QUEUE);
}
static void presentation_is_serialized(void) {
    prove_exclusion(ENGINE_SUBMIT, PRESENT, QUEUE);
}
static void debug_queue_operations_are_serialized(void) {
    prove_exclusion(ENGINE_SUBMIT, LABEL, QUEUE);
}
static void unregister_waits_for_driver_call_and_stale_wrappers_fail_closed(void) {
    PFN_vkQueueSubmit saved = (PFN_vkQueueSubmit) engine_proc("vkQueueSubmit");
    prove_exclusion(ENGINE_SUBMIT, UNREGISTER, QUEUE);
    TEST_ASSERT_EQUAL_INT(VK_ERROR_DEVICE_LOST, saved(QUEUE, 0, NULL, VK_NULL_HANDLE));
    TEST_ASSERT_EQUAL_INT(VK_ERROR_DEVICE_LOST, vk_queue_sync_device_wait_idle(DEVICE));
    TEST_ASSERT_EQUAL_INT(1, entered);
}
static void every_driver_entry_is_locked(void) {
    for (enum operation op = ENGINE_SUBMIT; op < UNREGISTER; op++)
        TEST_ASSERT_EQUAL_INT(VK_SUCCESS, call(op, QUEUE));
    ((PFN_vkQueueBeginDebugUtilsLabelEXT) engine_proc("vkQueueBeginDebugUtilsLabelEXT"))(QUEUE, NULL);
    ((PFN_vkQueueEndDebugUtilsLabelEXT) engine_proc("vkQueueEndDebugUtilsLabelEXT"))(QUEUE);
#ifdef VK_VERSION_1_3
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, ((PFN_vkQueueSubmit2) engine_proc("vkQueueSubmit2"))(QUEUE, 0, NULL, VK_NULL_HANDLE));
#endif
    TEST_ASSERT_EQUAL_INT(0, unlocked_entries);
}
static void both_resolver_paths_wrap_and_preserve_availability(void) {
    const char *names[] = { "vkQueueSubmit",
                            "vkQueueWaitIdle",
                            "vkDeviceWaitIdle",
                            "vkQueueBindSparse",
                            "vkQueuePresentKHR",
                            "vkQueueSubmit2KHR",
                            "vkQueueBeginDebugUtilsLabelEXT",
                            "vkQueueEndDebugUtilsLabelEXT",
                            "vkQueueInsertDebugUtilsLabelEXT" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        PFN_vkVoidFunction direct = vk_queue_sync_get_instance_proc_addr(INSTANCE, names[i]);
        TEST_ASSERT_TRUE(direct != NULL && direct != real_lookup(names[i]));
        TEST_ASSERT_TRUE(direct == engine_proc(names[i]));
    }
    TEST_ASSERT_TRUE(engine_proc("vkUnrelated") == (PFN_vkVoidFunction) unrelated);
    TEST_ASSERT_TRUE(engine_proc("vkNotAvailable") == NULL);
    TEST_ASSERT_TRUE(engine_proc("vkQueueFutureVendorCommand") == NULL);
    optional_available = false;
    TEST_ASSERT_TRUE(engine_proc("vkQueuePresentKHR") == NULL);
    TEST_ASSERT_TRUE(vk_queue_sync_get_instance_proc_addr(INSTANCE, "vkQueueSubmit2KHR") == NULL);
}
static void driver_errors_release_lock_and_preserve_result(void) {
    driver_result = VK_ERROR_DEVICE_LOST;
    TEST_ASSERT_EQUAL_INT(VK_ERROR_DEVICE_LOST, call(ENGINE_SUBMIT, QUEUE));
    driver_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    TEST_ASSERT_EQUAL_INT(VK_ERROR_OUT_OF_HOST_MEMORY, call(RUNNER_IDLE, QUEUE));
    driver_result = VK_SUCCESS;
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, call(RUNNER_SUBMIT, QUEUE));
    TEST_ASSERT_EQUAL_INT(3, entered);
    TEST_ASSERT_EQUAL_INT(0, unlocked_entries);
}
static void registration_is_reusable_and_duplicate_does_not_replace_dispatch(void) {
    TEST_ASSERT_EQUAL_INT(EEXIST, vk_queue_sync_register(DEVICE, QUEUE));
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, call(ENGINE_SUBMIT, QUEUE));
    vk_queue_sync_unregister(DEVICE);
    TEST_ASSERT_EQUAL_INT(0, vk_queue_sync_register(DEVICE, QUEUE));
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, call(RUNNER_SUBMIT, QUEUE));
    TEST_ASSERT_EQUAL_INT(2, entered);
}
static void registration_failure_is_clean_and_retryable(void) {
    vk_queue_sync_unregister(DEVICE);
    fail_registration_alloc = true;
    TEST_ASSERT_EQUAL_INT(ENOMEM, vk_queue_sync_register(DEVICE, QUEUE));
    fail_registration_alloc = false;
    missing_entry = "vkQueueSubmit";
    TEST_ASSERT_EQUAL_INT(ENOTSUP, vk_queue_sync_register(DEVICE, QUEUE));
    TEST_ASSERT_EQUAL_INT(VK_ERROR_DEVICE_LOST, call(RUNNER_IDLE, QUEUE));
    missing_entry = NULL;
    TEST_ASSERT_EQUAL_INT(0, vk_queue_sync_register(DEVICE, QUEUE));
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, call(ENGINE_SUBMIT, QUEUE));
}
static void submit_arguments_reach_the_correct_driver_unchanged(void) {
    VkSubmitInfo info = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkFence fence = HANDLE(VkFence, 42);
    TEST_ASSERT_EQUAL_INT(VK_SUCCESS, ((PFN_vkQueueSubmit) engine_proc("vkQueueSubmit"))(QUEUE2, 1, &info, fence));
    TEST_ASSERT_TRUE(submitted_queue == QUEUE2);
    TEST_ASSERT_EQUAL_UINT32(1, submitted_count);
    TEST_ASSERT_EQUAL_PTR(&info, submitted_infos);
    TEST_ASSERT_TRUE(submitted_fence == fence);
    TEST_ASSERT_EQUAL_INT(0, unlocked_entries);
}
int main(int argc, char **argv) {
    UNITY_BEGIN();
#define CASE(name)                            \
    if (argc == 1 || !strcmp(argv[1], #name)) \
    RUN_TEST(name)
    CASE(registration_failure_is_clean_and_retryable);
    CASE(submit_arguments_reach_the_correct_driver_unchanged);
    CASE(engine_submit_excludes_runner_submit);
    CASE(engine_upload_excludes_runner_device_idle);
    CASE(engine_device_idle_excludes_runner_submit);
    CASE(runner_device_idle_excludes_engine_upload);
    CASE(shared_lock_covers_all_device_queues);
    CASE(queue_wait_is_serialized);
    CASE(submit2_alias_is_serialized);
    CASE(sparse_bind_is_serialized);
    CASE(presentation_is_serialized);
    CASE(debug_queue_operations_are_serialized);
    CASE(unregister_waits_for_driver_call_and_stale_wrappers_fail_closed);
    CASE(every_driver_entry_is_locked);
    CASE(both_resolver_paths_wrap_and_preserve_availability);
    CASE(driver_errors_release_lock_and_preserve_result);
    CASE(registration_is_reusable_and_duplicate_does_not_replace_dispatch);
    return UNITY_END();
}
