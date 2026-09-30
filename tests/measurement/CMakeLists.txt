# SPDX-License-Identifier: Apache-2.0
#
# What the upcall round trip costs. Nothing here is registered with ctest: a probe measures rather
# than asserts, so a slow run is a result and not a failure.
#
# It still builds with the suite, so a change that breaks a probe breaks the build gate rather than
# waiting to be noticed the next time someone measures.

add_executable(probe_ops probe_ops.cpp)
target_link_libraries(probe_ops PRIVATE fs-testing)
target_include_directories(probe_ops PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel/include)
target_compile_options(probe_ops PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

add_executable(probe_access probe_access.cpp)
target_link_libraries(probe_access PRIVATE fs-testing)
target_include_directories(probe_access PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel/include)
target_compile_options(probe_access PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

add_executable(probe_perm probe_perm.cpp)
target_link_libraries(probe_perm PRIVATE fs-testing)
target_include_directories(probe_perm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel/include)
target_compile_options(probe_perm PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

add_executable(probe_data probe_data.cpp)
target_link_libraries(probe_data PRIVATE fs-testing)
target_include_directories(probe_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel/include)
target_compile_options(probe_data PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

# The floor, so it reaches neither the library nor the harness: a chardev has no names to join to a
# mount point and no turn counter to read.
add_executable(probe_devdax probe_devdax.cpp)
target_compile_options(probe_devdax PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
