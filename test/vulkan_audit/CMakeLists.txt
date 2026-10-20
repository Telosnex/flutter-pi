# Vulkan audit regressions. Select with CTest's vulkan-audit label.
if(HAVE_VULKAN)
    add_executable(vulkan_renderer_audit_test renderer_test.c)
    add_executable(vulkan_surface_audit_test surface_test.c ${PROJECT_SOURCE_DIR}/src/compositor_ng.c)
    foreach(target vulkan_renderer_audit_test vulkan_surface_audit_test)
        target_link_libraries(${target} lifecycle_test_support Unity)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    endforeach()
    foreach(symbol poll vk_renderer_get_physical_device vk_renderer_ref vk_renderer_unref vk_renderer_get_device vk_renderer_get_queue
            vk_renderer_supports_sync_fd_semaphore_export vk_renderer_find_mem_type drmdev_ref drmdev_unref
            drmdev_rm_fb drmdev_add_fb kms_req_builder_get_drmdev kms_req_builder_push_fb_layer window_push_composition)
        target_link_options(vulkan_surface_audit_test PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    foreach(case constructor_initializes_reference_count validation_callback_does_not_abort_vulkan_calls
            rejects_device_below_required_api_version rejects_device_with_no_extensions_without_undefined_behavior
            accepts_device_at_required_api_version
            constructor_failure_unwinds_created_handles memory_type_bit31_has_no_undefined_shift)
        add_test(NAME vulkan_audit.${case} COMMAND vulkan_renderer_audit_test ${case})
        set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endforeach()
    foreach(case bind_failure_destroys_image modifier_explicit_layout_uses_zero_size
            allocation_covers_image_memory_requirements surface_destruction_releases_renderer_reference
            last_front_buffer_does_not_keep_surface_alive failed_fence_export_does_not_resignal_binary_semaphore
            failed_device_wait_does_not_present_unsynchronized_image completed_sync_fd_export_does_not_wait_for_device
            rejected_kms_push_releases_fd_and_buffer_reference import_early_failures_release_resources
            partial_surface_initialization_unwinds_previous_images two_backing_layers_keep_distinct_images
            backing_layer_preserves_horizontal_offset compositor_propagates_queue_present_failure)
        add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
        set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endforeach()
endif()

if(HAVE_VULKAN)
    add_executable(vulkan_kms_audit_test kms_test.c)
    add_executable(vulkan_window_audit_test window_test.c)
    foreach(target vulkan_kms_audit_test vulkan_window_audit_test)
        target_link_libraries(${target} lifecycle_test_support Unity)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    endforeach()
    foreach(case atomic_plane_without_fence_must_not_silently_accept legacy_path_must_not_discard_explicit_fence
            failed_fence_property_must_fail_layer_submission supported_plane_receives_fence_and_retains_fd)
        add_test(NAME vulkan_audit.${case} COMMAND vulkan_kms_audit_test ${case})
        set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endforeach()
    foreach(case cached_surface_matches_requested_dimensions same_dimensions_can_reuse_cached_surface failed_resize_keeps_old_cache_alive)
        add_test(NAME vulkan_audit.${case} COMMAND vulkan_window_audit_test ${case})
        set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endforeach()
endif()

if(HAVE_VULKAN)
    get_property(requested_vulkan_debug CACHE VULKAN_DEBUG PROPERTY VALUE)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND requested_vulkan_debug STREQUAL "AUTO")
        add_test(NAME vulkan_audit.debug_auto_enables_validation
            COMMAND ${CMAKE_COMMAND} "-DACTUAL_VULKAN_DEBUG=${VULKAN_DEBUG}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/debug_config.cmake)
        set_tests_properties(vulkan_audit.debug_auto_enables_validation PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endif()
endif()

if(HAVE_VULKAN)
    foreach(symbol __next_plane drmdev_get_gbm_device vk_gbm_render_surface_new)
        target_link_options(vulkan_window_audit_test PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
endif()

if(HAVE_VULKAN)
    foreach(case failed_export_and_wait_never_reuse_signaled_semaphore
            unsupported_kms_fence_waits_then_retries_without_fence
            unsupported_kms_fence_with_failed_wait_does_not_present
            snapshot_pins_image_across_next_frame_and_engine_release)
        add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
        set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
    endforeach()
    add_test(NAME vulkan_audit.failed_property_rolls_back_request_plane_and_caller_ownership
        COMMAND vulkan_kms_audit_test failed_property_rolls_back_request_plane_and_caller_ownership)
    set_tests_properties(vulkan_audit.failed_property_rolls_back_request_plane_and_caller_ownership
        PROPERTIES LABELS vulkan-audit TIMEOUT 10)
endif()

# Deliberately NOT registered with CTest: only run explicitly on a hardware
# render node. Never takes DRM master or changes the display/service.
add_executable(vulkan_gbm_smoke gbm_smoke.c)
target_link_libraries(vulkan_gbm_smoke lifecycle_test_support)
set_property(TARGET vulkan_gbm_smoke PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

# Previously red follow-up regressions, now fixed.
foreach(case multiplane_modifier_is_supported_or_rejected_before_vulkan
        unsupported_pixel_format_is_rejected_without_abort_or_invalid_vulkan_call)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS "vulkan-audit;vulkan-audit-followup" TIMEOUT 10)
endforeach()
target_link_options(vulkan_window_audit_test PRIVATE "LINKER:--wrap=drm_plane_for_each_modified_format")
add_test(NAME vulkan_audit.modifier_allocation_failure_does_not_dereference_null
    COMMAND vulkan_window_audit_test modifier_allocation_failure_does_not_dereference_null)
set_tests_properties(vulkan_audit.modifier_allocation_failure_does_not_dereference_null
    PROPERTIES LABELS "vulkan-audit;vulkan-audit-followup" TIMEOUT 10)

foreach(case gl_modifier_allocation_failure_preserves_cache empty_modifier_list_does_not_require_allocation
        vulkan_surface_receives_plane_modifiers vulkan_surface_without_plane_modifiers_gets_null)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_window_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
endforeach()

# Source-review regressions: formerly red, now fixed (never WILL_FAIL).
foreach(case two_layer_three_frame_overlap_can_acquire_next_frame
        repaint_reuses_image_completion_without_new_queue_work
        teardown_without_export_waits_before_destroying_busy_images
        teardown_after_export_disabled_waits_for_later_work
        invalid_modifier_is_rejected_before_vulkan_image_creation
        fbdev_presentation_returns_error_instead_of_aborting
        non_axis_aligned_layer_is_rejected_instead_of_misrendered)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS "vulkan-audit;vulkan-audit-source-review" TIMEOUT 10)
endforeach()
add_test(NAME vulkan_audit.single_layer_three_frame_overlap_fits
    COMMAND vulkan_surface_audit_test single_layer_three_frame_overlap_fits)
set_tests_properties(vulkan_audit.single_layer_three_frame_overlap_fits PROPERTIES LABELS vulkan-audit TIMEOUT 10)

add_test(NAME vulkan_audit.teardown_with_export_waits_before_destroying_busy_image
    COMMAND vulkan_surface_audit_test teardown_with_export_waits_before_destroying_busy_image)
set_tests_properties(vulkan_audit.teardown_with_export_waits_before_destroying_busy_image PROPERTIES LABELS vulkan-audit TIMEOUT 10)

foreach(case capability_failures_reject_before_image_creation
        repaint_duplicates_cached_fence_and_keeps_it_alive
        rejected_present_preserves_cached_completion_for_retry
        updated_engine_store_renews_completion_without_closing_kms_fd
        recycled_slot_captures_new_completion
        no_export_repaint_does_not_repeat_device_wait
        no_kms_fence_repaint_does_not_wait_on_device
        snapshot_fbdev_returns_error_instead_of_crashing)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
endforeach()

foreach(case failed_fence_dup_preserves_completion_for_retry
        interrupted_fence_wait_retries_without_queue_access
        errored_sync_file_does_not_present
        failed_final_wait_retains_gpu_resources)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS vulkan-audit TIMEOUT 10)
endforeach()

if(HAVE_VULKAN)
    add_executable(vulkan_queue_sync_test queue_sync_test.c)
    target_link_libraries(vulkan_queue_sync_test Unity PkgConfig::VULKAN pthread)
    foreach(case registration_failure_is_clean_and_retryable submit_arguments_reach_the_correct_driver_unchanged
            engine_submit_excludes_runner_submit engine_upload_excludes_runner_device_idle
            engine_device_idle_excludes_runner_submit runner_device_idle_excludes_engine_upload
            shared_lock_covers_all_device_queues queue_wait_is_serialized submit2_alias_is_serialized
            sparse_bind_is_serialized presentation_is_serialized debug_queue_operations_are_serialized
            unregister_waits_for_driver_call_and_stale_wrappers_fail_closed every_driver_entry_is_locked
            both_resolver_paths_wrap_and_preserve_availability driver_errors_release_lock_and_preserve_result
            registration_is_reusable_and_duplicate_does_not_replace_dispatch)
        add_test(NAME vulkan_queue_sync.${case} COMMAND vulkan_queue_sync_test ${case})
        set_tests_properties(vulkan_queue_sync.${case} PROPERTIES LABELS "vulkan-audit;vulkan-queue-sync" TIMEOUT 10)
    endforeach()
endif()

# Open item #7: Vulkan allocates only with modifiers the display plane also accepts.
foreach(case no_common_plane_modifier_fails_cleanly common_linear_only_allocates_linear
        common_tiled_modifier_is_used allocator_ignoring_common_modifiers_is_rejected)
    add_test(NAME vulkan_audit.${case} COMMAND vulkan_surface_audit_test ${case})
    set_tests_properties(vulkan_audit.${case} PROPERTIES LABELS "vulkan-audit;vulkan-audit-modifiers" TIMEOUT 10)
endforeach()
