cmake_minimum_required(VERSION 3.20)
project(vk_dyn_test C)
# BO-231: the run-time Vulkan loader (src/d3d/vk_dyn.c). Standalone:
#   cmake -S tests/vk_dyn -B build/vk_dyn && cmake --build build/vk_dyn
#   ctest --test-dir build/vk_dyn
# Needs the Vulkan headers only. On macOS the same code runs (XR_VK_FORCE_DYNAMIC)
# so the tests run on the development machine.
set(CMAKE_C_STANDARD 11)
find_package(Vulkan REQUIRED)
set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# A stand-in for the Vulkan loader of a PC with an old driver: it answers the
# way a loader (or driver) without the Vulkan 1.3 names does, per FAKE_VK_MODE.
add_library(fake_vk SHARED fake_loader.c)
set_target_properties(fake_vk PROPERTIES PREFIX "")
target_include_directories(fake_vk PRIVATE ${Vulkan_INCLUDE_DIRS})
if(WIN32)
    target_compile_definitions(fake_vk PRIVATE FAKE_EXPORT=__declspec\(dllexport\))
else()
    target_compile_definitions(fake_vk PRIVATE "FAKE_EXPORT=__attribute__((visibility(\"default\")))")
endif()

add_executable(vk_dyn_test test_main.c ${RT}/d3d/vk_dyn.c ${RT}/platform/xr_log.c)
target_include_directories(vk_dyn_test PRIVATE ${Vulkan_INCLUDE_DIRS} ${RT} ${RT}/d3d ${RT}/platform)
if(APPLE)
    target_compile_definitions(vk_dyn_test PRIVATE XR_VK_FORCE_DYNAMIC=1)
endif()
if(NOT WIN32)
    target_link_libraries(vk_dyn_test PRIVATE ${CMAKE_DL_LIBS} pthread)
else()
    target_link_libraries(vk_dyn_test PRIVATE "-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic")
endif()

enable_testing()
add_test(NAME vk_dyn_missing COMMAND vk_dyn_test missing)
foreach(m full khr old11)
    add_test(NAME vk_dyn_${m} COMMAND vk_dyn_test ${m})
    set_tests_properties(vk_dyn_${m} PROPERTIES
        ENVIRONMENT "RECOMP_VULKAN_LOADER=$<TARGET_FILE:fake_vk>;FAKE_VK_MODE=${m}")
endforeach()
add_test(NAME vk_dyn_system COMMAND vk_dyn_test system)
set_tests_properties(vk_dyn_system PROPERTIES SKIP_RETURN_CODE 77)
