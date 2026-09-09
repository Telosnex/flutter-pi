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
