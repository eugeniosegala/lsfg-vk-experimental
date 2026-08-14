set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/gamescope_hdr_feedback.cpp"
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
    find_package(Threads REQUIRED)

    add_library(lsfg-vk-color STATIC
        "src/color_pipeline.cpp")
    target_include_directories(lsfg-vk-color
        PUBLIC "src")
    target_link_libraries(lsfg-vk-color
        PUBLIC lsfg-vk-backend)
    set_target_properties(lsfg-vk-color PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    add_library(lsfg-vk-layer SHARED ${LAYER_SOURCES})

    target_link_libraries(lsfg-vk-layer
        PRIVATE lsfg-vk-adaptive
        PRIVATE lsfg-vk-color
        PRIVATE Threads::Threads
        PUBLIC lsfg-vk-common
        PUBLIC lsfg-vk-backend)

    target_compile_options(lsfg-vk-layer PRIVATE
        -Wno-unknown-warning-option
        -Wno-unsafe-buffer-usage) # Array indexing

    target_compile_definitions(lsfg-vk-layer PRIVATE
        LSFGVK_BUILD_VERSION="${LSFGVK_VERSION}")

    set_target_properties(lsfg-vk-layer PROPERTIES
        CXX_VISIBILITY_PRESET hidden)
endif()

if(BUILD_TESTING)
    add_executable(lsfg-vk-profile-update-tests
        "tests/profile_update_tests.cpp")
    target_include_directories(lsfg-vk-profile-update-tests
        PRIVATE "src"
        PRIVATE "../lsfg-vk-common/include")
    add_test(NAME profile-update
        COMMAND lsfg-vk-profile-update-tests)

    add_executable(lsfg-vk-runtime-transition-tests
        "tests/runtime_transition_tests.cpp")
    target_include_directories(lsfg-vk-runtime-transition-tests
        PRIVATE "src")
    add_test(NAME runtime-transition
        COMMAND lsfg-vk-runtime-transition-tests)

    add_executable(lsfg-vk-presentation-policy-tests
        "tests/presentation_policy_tests.cpp")
    target_include_directories(lsfg-vk-presentation-policy-tests
        PRIVATE "src")
    add_test(NAME presentation-policy
        COMMAND lsfg-vk-presentation-policy-tests)

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

    if(LSFGVK_BUILD_VK_LAYER)
        add_executable(lsfg-vk-color-tests
            "tests/color_pipeline_tests.cpp")
        target_link_libraries(lsfg-vk-color-tests
            PRIVATE lsfg-vk-color)
        add_test(NAME color-pipeline
            COMMAND lsfg-vk-color-tests)
    endif()

    add_executable(lsfg-vk-adaptive-benchmark
        "tests/adaptive_scheduler_benchmark.cpp")
    target_link_libraries(lsfg-vk-adaptive-benchmark
        PRIVATE lsfg-vk-adaptive)

    add_executable(lsfg-vk-hdr-color-math-tests
        "tests/hdr_color_math_tests.cpp")
    add_test(NAME hdr-color-math
        COMMAND lsfg-vk-hdr-color-math-tests)
endif()

if(LSFGVK_BUILD_VK_LAYER)
    math(EXPR LSFGVK_LAYER_LIBRARY_ARCH "${CMAKE_SIZEOF_VOID_P} * 8")
    if(NOT LSFGVK_LAYER_LIBRARY_ARCH EQUAL 32 AND
            NOT LSFGVK_LAYER_LIBRARY_ARCH EQUAL 64)
        message(FATAL_ERROR
            "The Vulkan layer manifest supports only 32-bit or 64-bit libraries")
    endif()

    set(LSFGVK_LAYER_MANIFEST_FILE
        "VkLayer_LSFGVK_experimental_frame_generation${LSFGVK_LAYER_MANIFEST_SUFFIX}.json")
    configure_file(
        "VkLayer_LSFGVK_experimental_frame_generation.json.in"
        "${LSFGVK_LAYER_MANIFEST_FILE}"
        @ONLY)

    install(TARGETS lsfg-vk-layer
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${LSFGVK_LAYER_MANIFEST_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
endif()
