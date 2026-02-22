cmake_minimum_required(VERSION 3.16)
project(openEtherCAT VERSION 0.1.0 LANGUAGES CXX)

option(OEC_BUILD_EXAMPLES "Build examples" ON)
option(OEC_BUILD_TESTS "Build tests" ON)
option(OEC_BUILD_DOCS "Enable docs target" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(OEC_ENABLE_PACKAGING "Enable install + CPack packaging support" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(openethercat
    src/master/ethercat_master.cpp
    src/master/cycle_controller.cpp
    src/master/slave_diagnostics.cpp
    src/master/coe_mailbox.cpp
    src/master/distributed_clock.cpp
    src/master/foe_eoe.cpp
    src/master/hil_campaign.cpp
    src/master/topology_manager.cpp
    src/mapping/io_mapper.cpp
    src/config/eni_esi_models.cpp
    src/config/config_loader.cpp
    src/config/config_validator.cpp
    src/config/recovery_profile_loader.cpp
    src/transport/linux_raw_socket_transport.cpp
    src/transport/linux_raw_socket_transport_mailbox.cpp
    src/transport/linux_raw_socket_transport_foe_eoe.cpp
    src/transport/linux_raw_socket_transport_topology.cpp
    src/transport/linux_raw_socket_transport_process_image.cpp
    src/transport/linux_raw_socket_transport_state_dc.cpp
    src/transport/linux_raw_socket_transport_core_io.cpp
    src/transport/coe_mailbox_protocol.cpp
    src/transport/ethercat_frame.cpp
    src/transport/mock_transport.cpp
    src/transport/transport_factory.cpp
)

target_include_directories(openethercat
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(openethercat PUBLIC cxx_std_17)
set_target_properties(openethercat PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_compile_options(openethercat
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

if(OEC_BUILD_EXAMPLES)
    add_executable(beckhoff_io_demo examples/beckhoff_io_demo.cpp)
    target_link_libraries(beckhoff_io_demo PRIVATE openethercat)

    add_executable(periodic_controller_demo examples/periodic_controller_demo.cpp)
    target_link_libraries(periodic_controller_demo PRIVATE openethercat)

    add_executable(linux_raw_socket_cycle_demo examples/linux_raw_socket_cycle_demo.cpp)
    target_link_libraries(linux_raw_socket_cycle_demo PRIVATE openethercat)

    add_executable(recovery_diagnostics_demo diagnostics/recovery_diagnostics_demo.cpp)
    target_link_libraries(recovery_diagnostics_demo PRIVATE openethercat)

    add_executable(recovery_profile_demo examples/recovery_profile_demo.cpp)
    target_link_libraries(recovery_profile_demo PRIVATE openethercat)

    add_executable(coe_dc_topology_demo diagnostics/coe_dc_topology_demo.cpp)
    target_link_libraries(coe_dc_topology_demo PRIVATE openethercat)

    add_executable(mock_hil_soak examples/mock_hil_soak.cpp)
    target_link_libraries(mock_hil_soak PRIVATE openethercat)

    add_executable(hil_conformance_demo examples/hil_conformance_demo.cpp)
    target_link_libraries(hil_conformance_demo PRIVATE openethercat)

    add_executable(ds402_cubic_trajectory_demo examples/ds402_cubic_trajectory_demo.cpp)
    target_link_libraries(ds402_cubic_trajectory_demo PRIVATE openethercat)

    add_executable(el6692_bridge_demo examples/el6692_bridge_demo.cpp)
    target_link_libraries(el6692_bridge_demo PRIVATE openethercat)

    add_executable(el6692_structured_bridge_demo examples/el6692_structured_bridge_demo.cpp)
    target_link_libraries(el6692_structured_bridge_demo PRIVATE openethercat)

    add_executable(el6751_can_bridge_demo examples/el6751_can_bridge_demo.cpp)
    target_link_libraries(el6751_can_bridge_demo PRIVATE openethercat)

    add_executable(physical_topology_scan_demo diagnostics/physical_topology_scan_demo.cpp)
    target_link_libraries(physical_topology_scan_demo PRIVATE openethercat)

    add_executable(topology_reconcile_demo diagnostics/topology_reconcile_demo.cpp)
    target_link_libraries(topology_reconcile_demo PRIVATE openethercat)

    add_executable(redundancy_fault_sequence_demo diagnostics/redundancy_fault_sequence_demo.cpp)
    target_link_libraries(redundancy_fault_sequence_demo PRIVATE openethercat)

    add_executable(mailbox_soak_demo diagnostics/mailbox_soak_demo.cpp)
    target_link_libraries(mailbox_soak_demo PRIVATE openethercat)

    add_executable(dc_hardware_sync_demo diagnostics/dc_hardware_sync_demo.cpp)
    target_link_libraries(dc_hardware_sync_demo PRIVATE openethercat)

    add_executable(dc_soak_demo diagnostics/dc_soak_demo.cpp)
    target_link_libraries(dc_soak_demo PRIVATE openethercat)

    add_executable(topology_to_eni_dump diagnostics/topology_to_eni_dump.cpp)
    target_link_libraries(topology_to_eni_dump PRIVATE openethercat)

    add_executable(foe_eoe_smoke_demo diagnostics/foe_eoe_smoke_demo.cpp)
    target_link_libraries(foe_eoe_smoke_demo PRIVATE openethercat)
endif()

if(OEC_BUILD_TESTS)
    enable_testing()
    add_executable(mapping_tests tests/mapping_tests.cpp)
    target_link_libraries(mapping_tests PRIVATE openethercat)
    add_test(NAME mapping_tests COMMAND mapping_tests)

    add_executable(protocol_and_loader_tests tests/protocol_and_loader_tests.cpp)
    target_link_libraries(protocol_and_loader_tests PRIVATE openethercat)
    add_test(NAME protocol_and_loader_tests COMMAND protocol_and_loader_tests)

    add_executable(production_hardening_tests tests/production_hardening_tests.cpp)
    target_link_libraries(production_hardening_tests PRIVATE openethercat)
    add_test(NAME production_hardening_tests COMMAND production_hardening_tests)

    add_executable(advanced_systems_tests tests/advanced_systems_tests.cpp)
    target_link_libraries(advanced_systems_tests PRIVATE openethercat)
    add_test(NAME advanced_systems_tests COMMAND advanced_systems_tests)

    add_executable(coe_mailbox_protocol_tests tests/coe_mailbox_protocol_tests.cpp)
    target_link_libraries(coe_mailbox_protocol_tests PRIVATE openethercat)
    add_test(NAME coe_mailbox_protocol_tests COMMAND coe_mailbox_protocol_tests)

    add_executable(transport_module_boundary_tests tests/transport_module_boundary_tests.cpp)
    target_compile_definitions(transport_module_boundary_tests PRIVATE OEC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME transport_module_boundary_tests COMMAND transport_module_boundary_tests)
endif()

if(OEC_BUILD_DOCS)
    find_package(Doxygen QUIET)
    if(DOXYGEN_FOUND)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    else()
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found. Install doxygen and re-run this target."
            COMMENT "Docs target placeholder"
        VERBATIM
        )
    endif()
endif()

if(OEC_ENABLE_PACKAGING)
    include(CMakePackageConfigHelpers)

    set(OEC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/openethercat")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/openethercatConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/openethercatConfig.cmake"
        INSTALL_DESTINATION "${OEC_INSTALL_CMAKEDIR}"
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/openethercatConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(TARGETS openethercat
        EXPORT openethercatTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime NAMELINK_COMPONENT dev
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT dev
    )

    install(EXPORT openethercatTargets
        FILE openethercatTargets.cmake
        NAMESPACE openethercat::
        DESTINATION "${OEC_INSTALL_CMAKEDIR}"
        COMPONENT dev
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/openethercatConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/openethercatConfigVersion.cmake"
        DESTINATION "${OEC_INSTALL_CMAKEDIR}"
        COMPONENT dev
    )

    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_PACKAGE_NAME "openethercat")
    set(CPACK_PACKAGE_VENDOR "OpenEtherCAT")
    set(CPACK_PACKAGE_CONTACT "OpenEtherCAT Maintainers")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OpenEtherCAT Maintainers")
    set(CPACK_COMPONENTS_ALL runtime dev)

    set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "OpenEtherCAT Runtime")
    set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Runtime shared library for OpenEtherCAT")
    set(CPACK_COMPONENT_DEV_DISPLAY_NAME "OpenEtherCAT Development Files")
    set(CPACK_COMPONENT_DEV_DESCRIPTION "Headers and CMake package files for OpenEtherCAT")
    set(CPACK_COMPONENT_DEV_DEPENDS runtime)
    set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)

    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "libopenethercat")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)

    set(CPACK_DEBIAN_DEV_PACKAGE_NAME "openethercat-dev")
    set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
    set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS "libopenethercat (= ${CPACK_PACKAGE_VERSION})")

    include(CPack)
endif()
