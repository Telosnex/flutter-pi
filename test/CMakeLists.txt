add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(
    platformchannel_test
    flutterpi_module
    Unity
)

add_test(platformchannel_test platformchannel_test)

add_executable(flutterpi_test
    flutterpi_test.c
)

target_link_libraries(
    flutterpi_test
    flutterpi_module
    Unity
)

add_test(flutterpi_test flutterpi_test)

add_executable(frame_scheduler_test
    frame_scheduler_test.c
)

target_link_libraries(
    frame_scheduler_test
    flutterpi_module
    Unity
)

add_test(frame_scheduler_test frame_scheduler_test)

add_executable(window_test
    window_test.c
)

target_link_libraries(
    window_test
    flutterpi_module
    Unity
)

add_test(window_test window_test)

# Pure event translation: can also be compiled on a host without libinput/DRM.
add_executable(user_input_scroll_test
    user_input_scroll_test.c
    ${PROJECT_SOURCE_DIR}/src/user_input_scroll.c
)
target_include_directories(user_input_scroll_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party/flutter_embedder_header/include
)
target_link_libraries(user_input_scroll_test Unity)
add_test(user_input_scroll_test user_input_scroll_test)

# An archive also lets white-box tests include one implementation directly:
# its object is then not pulled a second time (flutterpi_module is OBJECT).
add_library(lifecycle_test_support STATIC $<TARGET_OBJECTS:flutterpi_module>)
target_link_libraries(lifecycle_test_support INTERFACE $<TARGET_PROPERTY:flutterpi_module,LINK_LIBRARIES>)
target_link_options(lifecycle_test_support INTERFACE $<TARGET_PROPERTY:flutterpi_module,LINK_OPTIONS>)
target_include_directories(lifecycle_test_support INTERFACE $<TARGET_PROPERTY:flutterpi_module,INCLUDE_DIRECTORIES>)
target_compile_options(lifecycle_test_support INTERFACE $<TARGET_PROPERTY:flutterpi_module,COMPILE_OPTIONS>)
target_compile_definitions(lifecycle_test_support INTERFACE $<TARGET_PROPERTY:flutterpi_module,COMPILE_DEFINITIONS>)

add_executable(surface_lifecycle_test surface_lifecycle_test.c)
target_link_libraries(surface_lifecycle_test lifecycle_test_support Unity)
foreach(symbol window_push_composition drmdev_ref drmdev_unref drmdev_rm_fb drmdev_add_fb_from_dmabuf kms_req_builder_get_drmdev kms_req_builder_push_fb_layer)
    target_link_options(surface_lifecycle_test PRIVATE "LINKER:--wrap=${symbol}")
endforeach()
set_property(TARGET surface_lifecycle_test PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_test(NAME surface_lifecycle_test COMMAND surface_lifecycle_test)

add_executable(window_repaint_test window_repaint_test.c)
target_link_libraries(window_repaint_test lifecycle_test_support Unity)
target_link_options(window_repaint_test PRIVATE "LINKER:--wrap=drmdev_create_request_builder")
set_property(TARGET window_repaint_test PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_test(NAME window_repaint_test COMMAND window_repaint_test)

# Compile the wrapped call sites without LTO: cross-module inlining bypasses
# --wrap and would call real DRM functions with the test's fake device.
target_sources(surface_lifecycle_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/dmabuf_surface.c
    ${PROJECT_SOURCE_DIR}/src/compositor_ng.c
)

if(WPEWEBKIT_FOUND AND LIBGSTREAMER_GL_FOUND)
    add_executable(webview_completion_test
        webview_completion_test.c
        ${PROJECT_SOURCE_DIR}/src/plugins/gstreamer_video_player/webview_gpu.c
    )
    target_link_libraries(webview_completion_test Unity $<TARGET_PROPERTY:flutterpi_module,LINK_LIBRARIES>)
    target_link_options(webview_completion_test PRIVATE $<TARGET_PROPERTY:flutterpi_module,LINK_OPTIONS>)
    target_include_directories(webview_completion_test PRIVATE $<TARGET_PROPERTY:flutterpi_module,INCLUDE_DIRECTORIES>)
    target_compile_options(webview_completion_test PRIVATE $<TARGET_PROPERTY:flutterpi_module,COMPILE_OPTIONS>)
    target_compile_definitions(webview_completion_test PRIVATE $<TARGET_PROPERTY:flutterpi_module,COMPILE_DEFINITIONS>)
    add_test(NAME webview_completion_test COMMAND webview_completion_test)
endif()
