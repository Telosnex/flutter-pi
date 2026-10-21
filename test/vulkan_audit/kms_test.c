// SPDX-License-Identifier: MIT
// Actual KMS builder. No ioctl: capture the atomic properties handed to libdrm.
#include <poll.h>

#include <sys/eventfd.h>
#include <unity.h>

#include "../../src/modesetting.c"

static struct drm_plane plane;
static struct drm_crtc crtc;
static struct drmdev dev;
static struct kms_req_builder builder;
static struct drm_plane planes[2];
static int fence_fd, fence_properties, reject_property, releases, atomic_cursor;
static int64_t blend_values[2];
int drmModeAtomicGetCursor(drmModeAtomicReq *req) {
    (void) req;
    return atomic_cursor;
}
void drmModeAtomicSetCursor(drmModeAtomicReq *req, int cursor) {
    (void) req;
    atomic_cursor = cursor;
}
#define FENCE_PROPERTY 77
#define BLEND_PROPERTY_BASE 90
int drmModeAtomicAddProperty(drmModeAtomicReq *req, uint32_t object, uint32_t property, uint64_t value) {
    (void) req;
    (void) object;
    if (property >= BLEND_PROPERTY_BASE && property < BLEND_PROPERTY_BASE + 2) {
        blend_values[property - BLEND_PROPERTY_BASE] = (int64_t) value;
    }
    if (property == FENCE_PROPERTY) {
        if (reject_property)
            return -ENOMEM;
        fence_properties++;
    }
    return atomic_cursor++;
}
static void release(void *data) {
    (void) data;
    releases++;
}
void setUp(void) {
    plane = (struct drm_plane) { .id = 10, .type = kPrimary_DrmPlaneType };
    plane.supported_formats[PIXFMT_ARGB8888] = true;
    plane.ids.in_fence_fd = FENCE_PROPERTY;
    crtc = (struct drm_crtc) { .id = 11 };
    dev = (struct drmdev) { .n_planes = 1, .planes = &plane };
    builder = (struct kms_req_builder) { .drmdev = &dev, .crtc = &crtc, .req = (void *) (uintptr_t) 1 };
    BITSET_SET(builder.available_planes, 0);
    // An unsignaled fd token, not a real sync_file. No kernel fence ioctl is
    // exercised: the test checks that it is passed through or the layer rejected.
    fence_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fence_fd);
    fence_properties = reject_property = releases = 0;
    blend_values[0] = blend_values[1] = -1;
    atomic_cursor = 5;
}
void tearDown(void) {
    for (int i = 0; i < builder.n_layers; i++)
        builder.layers[i].release_callback(builder.layers[i].release_callback_userdata);
    if (fcntl(fence_fd, F_GETFD) != -1)
        close(fence_fd);
}
static int push(void) {
    struct kms_fb_layer layer = { .drm_fb_id = 42,
                                  .format = PIXFMT_ARGB8888,
                                  .dst_w = 64,
                                  .dst_h = 64,
                                  .src_w = 64u << 16,
                                  .src_h = 64u << 16,
                                  .has_in_fence_fd = true,
                                  .in_fence_fd = fence_fd };
    return kms_req_builder_push_fb_layer(&builder, &layer, release, NULL, NULL, NULL);
}
static void atomic_plane_without_fence_must_not_silently_accept(void) {
    plane.ids.in_fence_fd = DRM_ID_NONE;
    // A safe implementation may reject this plane or wait for producer completion.
    // This token never signals, so successful unguarded acceptance is unsafe.
    TEST_ASSERT_NOT_EQUAL(0, push());
}
static void legacy_path_must_not_discard_explicit_fence(void) {
    builder.use_legacy = true;
    TEST_ASSERT_NOT_EQUAL(0, push());
}
static void failed_fence_property_must_fail_layer_submission(void) {
    reject_property = 1;
    TEST_ASSERT_NOT_EQUAL(0, push());
}
static void supported_plane_receives_fence_and_retains_fd(void) {
    TEST_ASSERT_EQUAL_INT(0, push());
    TEST_ASSERT_EQUAL_INT(1, fence_properties);
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(fence_fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(0, releases);
}
static void failed_property_rolls_back_request_plane_and_caller_ownership(void) {
    reject_property = 1;
    TEST_ASSERT_EQUAL_INT(ENOMEM, push());
    TEST_ASSERT_EQUAL_INT(5, atomic_cursor);
    TEST_ASSERT_EQUAL_INT(0, builder.n_layers);
    TEST_ASSERT_TRUE(BITSET_TEST(builder.available_planes, 0));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(fence_fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(0, releases);
    reject_property = 0;
    TEST_ASSERT_EQUAL_INT(0, push());
}
static int push_plain(void) {
    struct kms_fb_layer layer = { .drm_fb_id = 42,
                                  .format = PIXFMT_ARGB8888,
                                  .dst_w = 64,
                                  .dst_h = 64,
                                  .src_w = 64u << 16,
                                  .src_h = 64u << 16 };
    return kms_req_builder_push_fb_layer(&builder, &layer, release, NULL, NULL, NULL);
}
static void upper_planes_get_premultiplied_blend_mode(void) {
    // Two planes that both support every blend mode. The bottom layer must be
    // opaque ("None"). Every layer above it, including the cursor, must be set
    // to premultiplied explicitly, because KMS keeps a plane's last value.
    for (int i = 0; i < 2; i++) {
        planes[i] = (struct drm_plane) { .id = 20 + i, .type = i == 0 ? kPrimary_DrmPlaneType : kOverlay_DrmPlaneType };
        planes[i].supported_formats[PIXFMT_ARGB8888] = true;
        planes[i].has_blend_mode = true;
        planes[i].ids.pixel_blend_mode = BLEND_PROPERTY_BASE + i;
        planes[i].ids.in_fence_fd = DRM_ID_NONE;
        for (int m = 0; m < kCount_DrmBlendMode; m++)
            planes[i].supported_blend_modes[m] = true;
    }
    dev = (struct drmdev) { .n_planes = 2, .planes = planes };
    BITSET_SET(builder.available_planes, 1);
    TEST_ASSERT_EQUAL_INT(0, push_plain());
    TEST_ASSERT_EQUAL_INT(0, push_plain());
    TEST_ASSERT_EQUAL_INT64(kNone_DrmBlendMode, blend_values[0]);
    TEST_ASSERT_EQUAL_INT64(kPremultiplied_DrmBlendMode, blend_values[1]);
}
int main(int argc, char **argv) {
    UNITY_BEGIN();
    int selected = 0;
#define CASE(f)                                  \
    if (argc == 1 || strcmp(argv[1], #f) == 0) { \
        selected++;                              \
        RUN_TEST(f);                             \
    }
    CASE(failed_property_rolls_back_request_plane_and_caller_ownership)
    CASE(atomic_plane_without_fence_must_not_silently_accept)
    CASE(legacy_path_must_not_discard_explicit_fence)
    CASE(failed_fence_property_must_fail_layer_submission)
    CASE(supported_plane_receives_fence_and_retains_fd)
    CASE(upper_planes_get_premultiplied_blend_mode)
    return selected ? UNITY_END() : 2;
}
