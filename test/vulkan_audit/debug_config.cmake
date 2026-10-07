# Invoked only for Debug builds with VULKAN_DEBUG requested as AUTO.
if(NOT ACTUAL_VULKAN_DEBUG)
    message(FATAL_ERROR "VULKAN_DEBUG=AUTO disabled validation in a Debug build (case-sensitive build-type comparison)")
endif()
