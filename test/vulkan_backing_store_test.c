// SPDX-License-Identifier: MIT
// Exercise real Vulkan slot ownership with synthetic images, no GPU/DRM calls.
#include <unity.h>

#include "../src/vk_gbm_render_surface.c"

static struct vk_gbm_render_surface *s;
static FlutterBackingStore stores[VK_GBM_BUFFER_COUNT];
static void destroy_store(int i) {
    if (stores[i].vulkan.destruction_callback) {
        stores[i].vulkan.destruction_callback(stores[i].vulkan.user_data);
        stores[i].vulkan.destruction_callback = NULL;
    }
}
static void fill(int i) {
    stores[i] = (FlutterBackingStore) { .struct_size = sizeof(FlutterBackingStore) };
    TEST_ASSERT_EQUAL_INT(0, vk_gbm_render_surface_fill(&s->render_surface, &stores[i]));
}
static void present(int i) {
    TEST_ASSERT_EQUAL_INT(0, vk_gbm_render_surface_queue_present(&s->render_surface, &stores[i]));
}
void setUp(void) {
    struct tracer *tracer = tracer_new_with_stubs();
    s = calloc(1, sizeof(*s));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(0, render_surface_init(&s->render_surface, tracer, VEC2I(640, 480)));
    tracer_unref(tracer);
#ifdef DEBUG
    uuid_copy(&s->uuid, uuid);
#endif
    for (int i = 0; i < ARRAY_SIZE(s->locked_fbs); i++) {
        atomic_flag_clear(&s->locked_fbs[i].is_locked);
        atomic_init(&s->locked_fbs[i].flutter_owned, false);
        s->locked_fbs[i].fb = &s->fbs[i];
        s->fbs[i].fl_image = (FlutterVulkanImage) { .struct_size = sizeof(FlutterVulkanImage), .image = i + 1 };
    }
    memset(stores, 0, sizeof(stores));
}
void tearDown(void) {
    for (int i = 0; i < ARRAY_SIZE(stores); i++)
        destroy_store(i);
    // The owner's internal front pin does not retain the containing surface.
    set_front_fb(s, NULL);
    surface_unref(&s->surface);
}
static void test_discard_before_present_releases_slot(void) {
    fill(0);
    destroy_store(0);
    TEST_ASSERT_EQUAL_INT(0, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
}
static void test_flutter_owns_slot_until_destruction_callback(void) {
    fill(0);
    present(0);
    fill(1);
    present(1);
    TEST_ASSERT_GREATER_THAN_INT(0, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
    destroy_store(0);
    TEST_ASSERT_EQUAL_INT(0, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
}
static void test_represent_does_not_consume_another_owners_reference(void) {
    fill(0);
    present(0);
    present(0);
    TEST_ASSERT_EQUAL_INT(2, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
}
static void test_kms_keeps_image_after_flutter_destroys_store(void) {
    fill(0);
    present(0);
    struct locked_fb *kms = locked_fb_ref(s->front_fb);
    fill(1);
    present(1);
    destroy_store(0);
    TEST_ASSERT_EQUAL_INT(1, refcount_get_for_debug(&kms->n_refs));
    on_release_layer(kms);
    TEST_ASSERT_EQUAL_INT(0, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
}
static void test_discarded_frames_do_not_exhaust_pool(void) {
    for (int i = 0; i < 100; i++) {
        fill(0);
        destroy_store(0);
        TEST_ASSERT_EQUAL_INT(0, refcount_get_for_debug(&s->locked_fbs[0].n_refs));
    }
}
static void test_full_pool_fails_safely_and_recovers_after_release(void) {
    for (int i = 0; i < ARRAY_SIZE(stores); i++)
        fill(i);
    FlutterBackingStore extra = { 0 };
    TEST_ASSERT_EQUAL_INT(EAGAIN, vk_gbm_render_surface_fill(&s->render_surface, &extra));
    TEST_ASSERT_NULL(extra.vulkan.destruction_callback);
    destroy_store(2);
    fill(2);
    TEST_ASSERT_EQUAL_UINT64(3, stores[2].vulkan.image->image);
}
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_discard_before_present_releases_slot);
    RUN_TEST(test_flutter_owns_slot_until_destruction_callback);
    RUN_TEST(test_represent_does_not_consume_another_owners_reference);
    RUN_TEST(test_kms_keeps_image_after_flutter_destroys_store);
    RUN_TEST(test_discarded_frames_do_not_exhaust_pool);
    RUN_TEST(test_full_pool_fails_safely_and_recovers_after_release);
    return UNITY_END();
}
