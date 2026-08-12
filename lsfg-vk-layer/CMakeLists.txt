set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/instance.cpp"
    "src/swapchain.cpp")

add_library(lsfg-vk-adaptive STATIC
    "src/adaptive_scheduler.cpp")
target_include_directories(lsfg-vk-adaptive
    PUBLIC "src")
set_target_properties(lsfg-vk-adaptive PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(LSFGVK_BUILD_VK_LAYER)
    add_library(lsfg-vk-layer SHARED ${LAYER_SOURCES})

    target_link_libraries(lsfg-vk-layer
        PRIVATE lsfg-vk-adaptive
        PUBLIC lsfg-vk-common
        PUBLIC lsfg-vk-backend)

    target_compile_options(lsfg-vk-layer PRIVATE
        -Wno-unknown-warning-option
        -Wno-unsafe-buffer-usage) # Array indexing

    set_target_properties(lsfg-vk-layer PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
endif()

if(BUILD_TESTING)
    add_executable(lsfg-vk-adaptive-tests
        "tests/adaptive_scheduler_tests.cpp")
    target_link_libraries(lsfg-vk-adaptive-tests
        PRIVATE lsfg-vk-adaptive)
    add_test(NAME adaptive-scheduler
        COMMAND lsfg-vk-adaptive-tests)

    add_executable(lsfg-vk-adaptive-matrix
        "tests/adaptive_scheduler_matrix.cpp")
    target_link_libraries(lsfg-vk-adaptive-matrix
        PRIVATE lsfg-vk-adaptive)
    add_test(NAME adaptive-scheduler-matrix
        COMMAND lsfg-vk-adaptive-matrix)

    add_executable(lsfg-vk-adaptive-benchmark
        "tests/adaptive_scheduler_benchmark.cpp")
    target_link_libraries(lsfg-vk-adaptive-benchmark
        PRIVATE lsfg-vk-adaptive)
endif()

if(LSFGVK_BUILD_VK_LAYER)
    configure_file(
        "VkLayer_LSFGVK_frame_generation.json.in"
        "VkLayer_LSFGVK_frame_generation.json"
        @ONLY)

    install(TARGETS lsfg-vk-layer
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_LSFGVK_frame_generation.json"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
endif()
