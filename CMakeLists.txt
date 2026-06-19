cmake_minimum_required(VERSION 3.20)

project(AsasOS NONE)

file(
    GLOB_RECURSE ASAS_PROJECT_INPUTS
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*"
    "${CMAKE_SOURCE_DIR}/src/*"
    "${CMAKE_SOURCE_DIR}/tests/*"
    "${CMAKE_SOURCE_DIR}/build.ps1"
)

add_custom_target(
    asas ALL
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/build.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS ${ASAS_PROJECT_INPUTS}
    COMMENT "Building Asas OS bootloader, kernel, and boot image"
    VERBATIM
)

add_custom_target(
    qemu-test
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/Run-QemuBootTest.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS asas
    COMMENT "Running Asas OS QEMU boot test"
    VERBATIM
)
