# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# The daemon probes. Each is its own binary, so a case that wedges or dies takes only itself down and
# ctest names it. The harness under harness/ carries the PASS/FAIL/RESULT convention.

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

# daemon_probe(<name> <source>) builds one probe binary and registers it with ctest.
function(daemon_probe name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE daemon_core daemon_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

daemon_probe(daemon-wire-probe wire_test.cpp)
daemon_probe(daemon-notify-probe notify_test.cpp)
daemon_probe(daemon-channel-probe channel_test.cpp)
daemon_probe(daemon-observe-probe observe_test.cpp)
daemon_probe(daemon-spiffe-probe spiffe_test.cpp)
daemon_probe(daemon-selector-probe selector_test.cpp)
daemon_probe(daemon-local-provider-probe local_provider_test.cpp)
daemon_probe(daemon-spire-provider-probe spire_provider_test.cpp)
daemon_probe(daemon-serve-probe serve_test.cpp)

# config and audit link their own library.
function(daemon_config_probe name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE daemon_config daemon_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

daemon_config_probe(daemon-config-probe config_test.cpp)
daemon_config_probe(daemon-audit-probe audit_test.cpp)

# A security probe states a property the daemon should hold. One the daemon does not hold yet
# carries the security label, which is how a green gate excludes it until the fix lands.
daemon_probe(daemon-security-identity-probe security_identity_test.cpp)
daemon_config_probe(daemon-security-audit-probe security_audit_test.cpp)
set_tests_properties(daemon-security-identity-probe PROPERTIES LABELS security)

# The registry probe registers its own backend, so it links the registry and nothing else.
add_executable(daemon-backends-probe backends_test.cpp)
target_link_libraries(daemon-backends-probe PRIVATE daemon_backends daemon_warnings)
add_test(NAME daemon-backends-probe COMMAND daemon-backends-probe)

# The OPA backend core drives its decide path over a mock transport, so its probe needs no HTTP
# library and builds in every configuration.
add_executable(daemon-opa-probe opa_test.cpp)
target_link_libraries(daemon-opa-probe PRIVATE daemon_opa_core daemon_warnings)
add_test(NAME daemon-opa-probe COMMAND daemon-opa-probe)

# The transport probe stands a server up and drives the real exchange at it, so it builds only
# where the curl transport does.
if(DAEMON_ENABLE_OPA)
    add_executable(daemon-opa-transport-probe opa_transport_test.cpp)
    target_link_libraries(daemon-opa-transport-probe PRIVATE daemon_opa_transport daemon_warnings)
    add_test(NAME daemon-opa-transport-probe COMMAND daemon-opa-transport-probe)
endif()

# Where the policy server's endpoint is, read off the shipped files and off this host. A script and
# not a probe, because what it holds is the deployment and not a unit of the daemon.
add_test(NAME test_opa_unix_socket
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/tests/test_opa_unix_socket.sh")
set_tests_properties(test_opa_unix_socket PROPERTIES SKIP_RETURN_CODE 77)

# Who may read the config, the identity rules and the policy. A script for the same reason: the
# files it checks are the installer's, not anything this daemon builds.
add_test(NAME test_config_mode
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/tests/test_config_mode.sh")
set_tests_properties(test_config_mode PROPERTIES SKIP_RETURN_CODE 77)

# A SIGHUP that reads an edit, on a running instance. It edits the files and signals that instance,
# so it needs sudo and runs alone.
add_test(NAME test_config_reload
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/tests/test_config_reload.sh")
set_tests_properties(test_config_reload PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL ON)

# The policy probes link the policy library, so they build only when it does.
if(DAEMON_ENABLE_POLICY_REGORUS)
    function(daemon_policy_probe name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE daemon_policy daemon_warnings)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    daemon_policy_probe(daemon-policy-probe policy_test.cpp)
    # The probe reads the example a deployment copies, so a drift in either shows up as a failure.
    target_compile_definitions(daemon-policy-probe PRIVATE
        DAEMON_EXAMPLE_POLICY="${CMAKE_CURRENT_SOURCE_DIR}/../deploy/policy/policy.example.rego.in")

    # The access path drives the serve loop with a real policy, so it links both libraries.
    add_executable(daemon-access-probe access_test.cpp)
    target_link_libraries(daemon-access-probe PRIVATE daemon_core daemon_policy daemon_warnings)
    add_test(NAME daemon-access-probe COMMAND daemon-access-probe)
endif()

# The turn probes need a real cme region, so they build only when the turn backend does.
if(DAEMON_ENABLE_TURN)
    add_executable(daemon-lock-probe lock_test.cpp)
    target_link_libraries(daemon-lock-probe PRIVATE daemon_turn daemon_warnings)
    add_test(NAME daemon-lock-probe COMMAND daemon-lock-probe)

    add_executable(daemon-serve-lock-probe serve_lock_test.cpp)
    target_link_libraries(daemon-serve-lock-probe PRIVATE daemon_core daemon_turn daemon_warnings)
    add_test(NAME daemon-serve-lock-probe COMMAND daemon-serve-lock-probe)
endif()
