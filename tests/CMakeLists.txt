# SPDX-License-Identifier: Apache-2.0
#
# The suite. Every case is a binary over libroof, because that is the surface an application has:
# driving a mount with touch and rm measures the shell, not the contract.
#
# Nothing here runs without mounts and a daemon behind them, so a bare ctest run selects nothing and
# a label picks what a host can serve:
#
#   ctest -L onenode      one mount
#   ctest -L multinode    two mounts of one device
#   ctest -L manynode     every mount the host provisioned, however many that is
#   ctest -L gpu          one mount, and a GPU the CUDA runtime can see

set(FS_TEST_MOUNT "/mnt/${FS_NAME}" CACHE STRING "Mount a one-node case is pointed at")
set(FS_TEST_MOUNT_2 "/mnt/${FS_NAME}2" CACHE STRING "Second mount of the same device")
# A host that provisions more mounts adds them here, up to the slot table's FS_MAX_NODE_ID.
set(FS_TEST_MOUNTS "${FS_TEST_MOUNT};${FS_TEST_MOUNT_2}"
    CACHE STRING "Every provisioned mount of the device")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# @label decides which mounts the case is handed, so a case is never told twice how many it wants.
# Anything after it is another label it carries.
function(fs_add_case name label)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE fs-testing)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

    if(label STREQUAL "multinode")
        add_test(NAME ${name} COMMAND ${name} ${FS_TEST_MOUNT} ${FS_TEST_MOUNT_2})
    elseif(label STREQUAL "manynode")
        add_test(NAME ${name} COMMAND ${name} ${FS_TEST_MOUNTS})
    else()
        add_test(NAME ${name} COMMAND ${name} ${FS_TEST_MOUNT})
    endif()
    # A case that cannot reach what it claims says so with 77 rather than passing on nothing.
    set(labels ${label} ${ARGN})
    set_tests_properties(${name} PROPERTIES LABELS "${labels}" SKIP_RETURN_CODE 77)
endfunction()

if(NOT FS_TESTING)
    message(FATAL_ERROR "the suite needs FS_TESTING=ON: cases reach states the library refuses to produce")
endif()

# An example rather than a case, so only its default mode is registered. Two mounts of one device
# is the shape that can collide at all, since placement reads the extent map every node writes.
add_test(NAME test_overlap
         COMMAND fs-overlap ${FS_TEST_MOUNT} ${FS_TEST_MOUNT_2} --rounds 5)
set_tests_properties(test_overlap PROPERTIES LABELS multinode SKIP_RETURN_CODE 77 RUN_SERIAL ON)

# Not a case: test_postexec_attack execs into it, and the inode a delegation binds to needs a second
# executable to separate.
add_executable(postexec_other postexec_other.cpp)
target_link_libraries(postexec_other PRIVATE fs-testing)
target_include_directories(postexec_other PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)
target_compile_options(postexec_other PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

# Puts every mount on the metadata domain at once, so a failure elsewhere in the same run could not
# be told from the contention this produces.
fs_add_case(mount_stress manynode)
set_tests_properties(mount_stress PROPERTIES RUN_SERIAL ON)

fs_add_case(test_cache_policy onenode)
fs_add_case(test_channel_device onenode)
fs_add_case(test_cloexec_required onenode)
fs_add_case(test_contract onenode)
fs_add_case(test_cross_process multinode)
fs_add_case(test_dead_owner_unlink multinode)
fs_add_case(test_dupname multinode)
fs_add_case(test_exec_generation multinode)
fs_add_case(test_fork_inherit onenode)
fs_add_case(test_gc_deleg multinode)
fs_add_case(test_gc_owner multinode)

# A case that runs a second task inside one process needs the thread library, which is asked for
# here rather than at the top where every other case would carry it.
find_package(Threads REQUIRED)

# Holds one interval of the node's upcall count to no rise at all, so a case reaching the same node
# beside it would spend an upcall this one reads as its own.
fs_add_case(test_gc_thread_owner onenode)
target_link_libraries(test_gc_thread_owner PRIVATE Threads::Threads)
set_tests_properties(test_gc_thread_owner PROPERTIES RUN_SERIAL ON)

fs_add_case(test_lock_region_guard onenode)

# Counts the turns taken on the shared lock region, so a case running beside it spends turns this
# one reads as its own.
fs_add_case(test_meta_lock multinode)
set_tests_properties(test_meta_lock PROPERTIES RUN_SERIAL ON)

fs_add_case(test_mmap_bytes onenode)
fs_add_case(test_mmap_peer multinode)
fs_add_case(test_mmap_unplaced onenode)
fs_add_case(test_names onenode)
fs_add_case(test_names_concurrent multinode)
fs_add_case(test_names_peer multinode)
fs_add_case(test_perm_grants onenode)
fs_add_case(test_perm_peer multinode)
fs_add_case(test_perm_revoke multinode)
fs_add_case(test_pid_reuse multinode root)
fs_add_case(test_postexec_attack multinode pathrule)

# Takes every free slot in the region while it runs.
fs_add_case(test_rat_exhaustion onenode)
set_tests_properties(test_rat_exhaustion PROPERTIES RUN_SERIAL ON)

fs_add_case(test_sysfs multinode)

# Mis-stamps one node's tick under test/stamp_offset and reads the other's slot table, so a case
# beside it would see a fault or a slot state this one is in the middle of asserting on.
fs_add_case(test_stamp_fallback multinode root)
set_tests_properties(test_stamp_fallback PROPERTIES RUN_SERIAL ON)

# Needs CAP_SETUID to give one thread an account of its own, and reports a skip without it.
fs_add_case(test_thread_account onenode root)
target_link_libraries(test_thread_account PRIVATE Threads::Threads)

fs_add_case(test_thread_identity multinode)
target_link_libraries(test_thread_identity PRIVATE Threads::Threads)

fs_add_case(test_unlink_live_refs multinode)
fs_add_case(test_unlink_holders multinode)
fs_add_case(test_gc_owner_readers multinode)

# Churns names and slots on both nodes for its whole run, so a case reading region_info beside it
# would see rows come and go.
fs_add_case(test_unlink_race multinode)
set_tests_properties(test_unlink_race PROPERTIES RUN_SERIAL ON)

# Claims that the next placement lands on the extent the sweep just took back, which only holds
# while nothing else is placing.
fs_add_case(test_gc_zero_extent onenode)
set_tests_properties(test_gc_zero_extent PROPERTIES RUN_SERIAL ON)

fs_add_case(test_unmount_drain onenode root)

# Holds this node's drain closed for over a second, and any other prepare in that time is refused.
fs_add_case(test_unmount_drain_race onenode root)
set_tests_properties(test_unmount_drain_race PROPERTIES RUN_SERIAL ON)

fs_add_case(test_vm_protect onenode)
fs_add_case(test_vm_protect_peer multinode)

# The security cases. Each states a property the access path should hold, and each carries the
# security label, which is how a gate picks them up or leaves them out as one group.
#
#   ctest -L security
#
# test_security_helper_latch stops the daemon for longer than the kernel waits. It refuses to run
# unless SECURITY_HELPER_PID names the process it may stop, so a host that has not agreed
# reports a skip, and under sudo it creates as the invoking account.
foreach(securityCase test_security_deleg_revoke test_security_group_revoke
                     test_security_gid_revoke
                     test_security_cloexec_cost test_security_cloexec_alias
                     test_security_helper_latch test_security_double_answer
                     test_security_probe_gate test_security_daemon_down
                     test_security_ns_revoke)
    add_executable(${securityCase} ${securityCase}.cpp)
    target_link_libraries(${securityCase} PRIVATE fs-testing)
    target_include_directories(${securityCase} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)
    target_compile_options(${securityCase} PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)
    add_test(NAME ${securityCase} COMMAND ${securityCase} ${FS_TEST_MOUNT})
    set_tests_properties(${securityCase} PROPERTIES LABELS security SKIP_RETURN_CODE 77)
endforeach()

# Holds one node's whole channel and leaves any non-ACCESS upcall on it unanswered, so a case
# sharing that node at the same time would time out for reasons this case did not cause.
set_tests_properties(test_security_double_answer PROPERTIES RUN_SERIAL ON)

# Measures a ratio, so another case competing for the same core would change what it reads.
set_tests_properties(test_security_cloexec_cost PROPERTIES RUN_SERIAL ON)

# Holds one interval of the node's upcall count to no rise at all, so a case mapping on the same
# node beside it would spend an upcall this one reads as its own.
set_tests_properties(test_security_deleg_revoke PROPERTIES RUN_SERIAL ON)

# Holds one node's channel with its helper latched and samples the depth beside the drive, so a case
# asking the same node at the same time would be counted as this one's own caller.
set_tests_properties(test_security_probe_gate PROPERTIES RUN_SERIAL ON)
target_link_libraries(test_security_probe_gate PRIVATE Threads::Threads)

# Takes one node's daemon away for its whole run, so every other case on that node would be refused
# for a reason this one caused.
set_tests_properties(test_security_daemon_down PROPERTIES RUN_SERIAL ON)

# dl only, so the CUDA runtime arrives at run time or not at all.
fs_add_case(test_mmap_cuda gpu)
target_link_libraries(test_mmap_cuda PRIVATE dl)

# Not a case: bootstrap_chaos runs it to put a live mapping on the other side of a revoke. It opens
# through fs-testing, which reaches the kernel with a raw descriptor and no session behind it.
add_executable(chaos_mapper chaos_mapper.cpp)
target_link_libraries(chaos_mapper PRIVATE fs-testing)
target_include_directories(chaos_mapper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../kernel/include)
target_compile_options(chaos_mapper PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion)

# Measures rather than asserts, so it is built and not registered. Its own directory carries what
# drives it.
add_subdirectory(measurement)

# Owns the module and takes the host's mounts down for its whole run, so nothing else may be running
# while it does. Needs root:
#   sudo ctest --test-dir <build> -L lifecycle
add_test(NAME bootstrap_chaos COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bootstrap_chaos.sh)
set_tests_properties(bootstrap_chaos PROPERTIES LABELS lifecycle SKIP_RETURN_CODE 77 RUN_SERIAL ON)

# Kills one node's daemon and remounts that node four times over, on the host's own mounts. Needs
# root and a module with CONFIG_FS_TEST_KNOBS.
add_test(NAME rat_summary_crash COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rat_summary_crash.sh)
set_tests_properties(rat_summary_crash PROPERTIES LABELS lifecycle SKIP_RETURN_CODE 77 RUN_SERIAL ON)

# The decisions the deploy scripts record and read back, on scratch files. No root and no mount.
add_test(NAME test_deploy_state COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_deploy_state.sh)

# Fills both mounts from both nodes at once and reads every extent while they stand, so nothing
# else may be placing meanwhile. No root.
add_test(NAME rat_place_race COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rat_place_race.sh)
set_tests_properties(rat_place_race PROPERTIES LABELS lifecycle SKIP_RETURN_CODE 77 RUN_SERIAL ON)

target_compile_definitions(test_postexec_attack PRIVATE
    FS_POSTEXEC_OTHER="$<TARGET_FILE:postexec_other>")
add_dependencies(test_postexec_attack postexec_other)
