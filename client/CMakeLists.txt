if(DEFINED ENV{LIBS_DIR})
    set(_LIBS_DIR "$ENV{LIBS_DIR}")
else()
    get_filename_component(_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../someip/commonapi-qnx" ABSOLUTE)
endif()

if(DEFINED ENV{OUTPUT_DIR})
    set(_OUTPUT_DIR "$ENV{OUTPUT_DIR}")
else()
    set(_OUTPUT_DIR "${_LIBS_DIR}/build-rpi")
endif()

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${_OUTPUT_DIR}/toolchain.cmake")
endif()

cmake_minimum_required(VERSION 3.13)
project(MotorDataClient C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

set(_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../interface")

list(APPEND CMAKE_PREFIX_PATH "${_OUTPUT_DIR}")
link_directories("${_OUTPUT_DIR}/lib")
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-rpath-link,${_OUTPUT_DIR}/lib")

find_package(CommonAPI         CONFIG REQUIRED)
find_package(CommonAPI-SomeIP  CONFIG REQUIRED)
find_package(vsomeip3          CONFIG REQUIRED)

set(_GEN_CORE_DIR    "${CMAKE_BINARY_DIR}/generated/core")
set(_GEN_SOMEIP_DIR  "${CMAKE_BINARY_DIR}/generated/someip")

find_program(CORE_GEN_EXE
    commonapi-core-generator-linux-x86_64
    PATHS "${_OUTPUT_DIR}/generators/core"
    REQUIRED)

find_program(SOMEIP_GEN_EXE
    commonapi-someip-generator-linux-x86_64
    PATHS "${_OUTPUT_DIR}/generators/someip"
    REQUIRED)

message(STATUS "Core generator:   ${CORE_GEN_EXE}")
message(STATUS "SOME/IP generator: ${SOMEIP_GEN_EXE}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GEN_CORE_DIR}"
    COMMAND "${CORE_GEN_EXE}" -sk -d "${_GEN_CORE_DIR}" "${_INTERFACE_DIR}/MotorDataService.fidl"
    RESULT_VARIABLE _core_result
)
if(NOT _core_result EQUAL 0)
    message(FATAL_ERROR "Core code generation failed (exit ${_core_result})")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GEN_SOMEIP_DIR}"
    COMMAND "${SOMEIP_GEN_EXE}" -d "${_GEN_SOMEIP_DIR}" "MotorDataService.fdepl"
    WORKING_DIRECTORY "${_INTERFACE_DIR}"
    RESULT_VARIABLE _someip_result
)
if(NOT _someip_result EQUAL 0)
    message(FATAL_ERROR "SOME/IP code generation failed (exit ${_someip_result})")
endif()

message(STATUS "Generated code in ${CMAKE_BINARY_DIR}/generated/")

include_directories(
    "${_GEN_CORE_DIR}"
    "${_GEN_SOMEIP_DIR}"
)

file(GLOB_RECURSE CORE_PROXY_SRCS
    "${_GEN_CORE_DIR}/v0/commonapi/*Proxy*.cpp")

file(GLOB_RECURSE SOMEIP_SRCS
    "${_GEN_SOMEIP_DIR}/v0/commonapi/*.cpp")

add_executable(MotorDataClient
    src/MotorDataClient.cpp
    src/shm_reader.c
    src/ai_result_writer.c
    ${CORE_PROXY_SRCS}
    ${SOMEIP_SRCS}
)

target_include_directories(MotorDataClient PRIVATE src)

target_link_libraries(MotorDataClient
    CommonAPI
    CommonAPI-SomeIP
    vsomeip3
)

install(TARGETS MotorDataClient DESTINATION bin)
install(FILES vsomeip_multicast.json DESTINATION bin RENAME vsomeip-client.json)
