# SPDX-License-Identifier: Apache-2.0
#
# Puts the tree's names into FS_NAME, DAEMON_NAME and LIB_NAME, each with an upper-case form for the
# identifiers derived from it. Locates the file from this module rather than from the caller, so a
# subdirectory configured on its own reads the same lines the top-level configure does.

set(_fs_name_upper_of_FS_NAME FS_UPPER)
set(_fs_name_upper_of_DAEMON_NAME DAEMON_UPPER)
set(_fs_name_upper_of_LIB_NAME LIB_UPPER)
foreach(_fs_key FS_NAME DAEMON_NAME LIB_NAME)
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../fsname" _fs_name_line REGEX "^${_fs_key}=")
    if(NOT _fs_name_line)
        message(FATAL_ERROR "fsname carries no ${_fs_key}= line")
    endif()
    string(REGEX REPLACE "^${_fs_key}=" "" ${_fs_key} "${_fs_name_line}")
    string(TOUPPER "${${_fs_key}}" ${_fs_name_upper_of_${_fs_key}})
    unset(_fs_name_upper_of_${_fs_key})
endforeach()
unset(_fs_key)
unset(_fs_name_line)

# Every kernel/include template has its rendered header beside it, so an editor and a fresh checkout
# resolve #include "uapi.h" off the same -I the compiler gets. This renders one only when it is
# missing: a header somebody already wrote code against is never rewritten under them, and a pair
# that disagrees stops the configure instead.
get_filename_component(FS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FS_GENERATED_INCLUDE "${FS_ROOT}/kernel/include")
set(FS_RENDER "${FS_ROOT}/tools/render-fsname.sh")

file(GLOB _fs_templates "${FS_GENERATED_INCLUDE}/*.h.in")
foreach(_fs_template IN LISTS _fs_templates)
    string(REGEX REPLACE "\\.in$" "" _fs_header "${_fs_template}")
    execute_process(COMMAND "${FS_RENDER}" --ensure "${_fs_template}" "${_fs_header}"
                    RESULT_VARIABLE _fs_rendered
                    OUTPUT_VARIABLE _fs_report ERROR_VARIABLE _fs_report)
    if(NOT _fs_rendered EQUAL 0)
        message(FATAL_ERROR "${_fs_report}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fs_template}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FS_ROOT}/fsname")
unset(_fs_templates)
unset(_fs_template)
unset(_fs_header)
unset(_fs_rendered)
unset(_fs_report)

# In the tree the C++ namespace is fsuser, which no application is likely to have taken. What ships
# carries the library's own name instead, and the library's sources are rewritten for that build
# rather than only its headers: a namespace is part of a mangled symbol, so publishing one name over
# an archive built under another would not link. The suite is written against the neutral name and
# is left out of such a build.
option(FS_PUBLISH_NAMESPACE "Build under the library's namespace rather than the neutral one" OFF)
