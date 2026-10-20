// SPDX-License-Identifier: MIT
// Opt-in hardware smoke: real GBM/Vulkan allocation, clear, and CPU readback.
// No Flutter engine, KMS commit, master acquisition, or service restart.
#include <stdio.h>

#include <fcntl.h>

#include "../../src/vk_gbm_render_surface.c"

static int clear_and_check(struct vk_renderer *r, struct fb *fb) {
    VkDevice d = vk_renderer_get_device(r);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd;
    int result = 1;
    VkResult ok = vkCreateCommandPool(
        d,
        &(VkCommandPoolCreateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                     .queueFamilyIndex = vk_renderer_get_queue_family_index(r) },
        NULL,
        &pool
    );
    if (ok != VK_SUCCESS)
        goto out;
    ok = vkAllocateCommandBuffers(
        d,
        &(VkCommandBufferAllocateInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1 },
        &cmd
    );
    if (ok != VK_SUCCESS)
        goto out;
    ok = vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo) { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO });
    if (ok != VK_SUCCESS)
        goto out;
    VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
    VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .image = fb->image,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .subresourceRange = range };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkClearColorValue red = { .float32 = { 1, 0, 0, 1 } };
    vkCmdClearColorImage(cmd, fb->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    ok = vkEndCommandBuffer(cmd);
    if (ok != VK_SUCCESS)
        goto out;
    // Exercise the same nested proc-address path used by Flutter/Vulkan-Hpp,
    // then use the runner's direct synchronized wait below.
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr
    ) vk_queue_sync_get_instance_proc_addr(vk_renderer_get_instance(r), "vkGetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr gdpa = gipa ? (PFN_vkGetDeviceProcAddr) gipa(vk_renderer_get_instance(r), "vkGetDeviceProcAddr") : NULL;
    PFN_vkQueueSubmit submit = gdpa ? (PFN_vkQueueSubmit) gdpa(d, "vkQueueSubmit") : NULL;
    if (!submit)
        goto out;
    ok = submit(
        vk_renderer_get_queue(r),
        1,
        &(VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd },
        VK_NULL_HANDLE
    );
    if (ok != VK_SUCCESS)
        goto out;
    ok = vk_queue_sync_device_wait_idle(d);
    if (ok != VK_SUCCESS)
        goto out;
    uint32_t stride;
    void *map = NULL;
    unsigned char *pixels = gbm_bo_map(fb->bo, 0, 0, 64, 64, GBM_BO_TRANSFER_READ, &stride, &map);
    if (!pixels) {
        fprintf(stderr, "GBM CPU readback unavailable\n");
        goto out;
    }
    result = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            unsigned char *p = pixels + y * stride + x * 4;
            if (p[0] != 0 || p[1] != 0 || p[2] != 255 || p[3] != 255)
                result = 1;
        }
    gbm_bo_unmap(fb->bo, map);
    if (result)
        fprintf(stderr, "Red BGRA pixel readback mismatch\n");
out:
    if (ok != VK_SUCCESS)
        fprintf(stderr, "Vulkan clear failed: %s\n", vk_strerror(ok));
    if (pool != VK_NULL_HANDLE) {
        vk_queue_sync_device_wait_idle(d);
        vkDestroyCommandPool(d, pool, NULL);
    }
    return result;
}
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/dri/renderD128\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open DRM render node");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(fd);
    struct tracer *tracer = tracer_new_with_stubs();
    struct vk_renderer *renderer = gbm ? vk_renderer_new() : NULL;
    int result = 1;
    if (!renderer)
        goto out;
    for (int cycle = 0; cycle < 20; cycle++) {
        struct vk_gbm_render_surface *s = vk_gbm_render_surface_new(tracer, VEC2I(64, 64), gbm, renderer, PIXFMT_ARGB8888, NULL, 0);
        if (!s)
            goto out;
        FlutterBackingStore store = { 0 };
        if (render_surface_fill(&s->render_surface, &store) != 0) {
            surface_unref(&s->surface);
            goto out;
        }
        int failed = clear_and_check(renderer, ((struct locked_fb *) store.vulkan.user_data)->fb);
        if (!failed)
            failed = render_surface_queue_present(&s->render_surface, &store);
        struct surface *snapshot = failed ? NULL : render_surface_get_present_surface(&s->render_surface, &store);
        if (!snapshot)
            failed = 1;
        store.vulkan.destruction_callback(store.vulkan.user_data);
        surface_unref(&s->surface);  // snapshot must keep the old image/owner alive
        if (snapshot)
            surface_unref(snapshot);  // last owner, including internal front pin, now tears down
        if (failed)
            goto out;
    }
    puts("PASS: 20 real GBM/Vulkan allocate/clear/readback/snapshot/teardown cycles");
    result = 0;
out:
    if (renderer)
        vk_renderer_unref(renderer);
    tracer_unref(tracer);
    if (gbm)
        gbm_device_destroy(gbm);
    close(fd);
    return result;
}
