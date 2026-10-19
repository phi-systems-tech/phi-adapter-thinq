cmake_minimum_required(VERSION 3.21)

project(phi-adapter-thinq VERSION 0.1.0 DESCRIPTION "Phi LG ThinQ appliance adapter" LANGUAGES CXX)

include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json 3.11 REQUIRED)
# The CA, the CSR signing and the route service's TLS listener are this
# package's own: the SDK keeps its OpenSSL private.
find_package(OpenSSL REQUIRED)

# MQTT from libmosquitto driven by the SDK's loop, as in phi-adapter-z2m.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO IMPORTED_TARGET libmosquitto)
endif()
if(TARGET PkgConfig::MOSQUITTO)
    add_library(mosquitto::mosquitto ALIAS PkgConfig::MOSQUITTO)
else()
    find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h)
    find_library(MOSQUITTO_LIBRARY mosquitto)
    if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
        message(FATAL_ERROR "libmosquitto not found. Install libmosquitto-dev.")
    endif()
    add_library(mosquitto::mosquitto UNKNOWN IMPORTED)
    set_target_properties(mosquitto::mosquitto PROPERTIES
        IMPORTED_LOCATION "${MOSQUITTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MOSQUITTO_INCLUDE_DIR}")
endif()

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout")
option(PHI_ADAPTER_THINQ_USE_LOCAL_ADAPTER_SDK "Use local ../phi-adapter-sdk" ON)

if(NOT TARGET phi::adapter-sdk)
    if(PHI_ADAPTER_THINQ_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
        set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
    else()
        find_package(phi-adapter-sdk CONFIG REQUIRED)
    endif()
endif()

# The wire without a socket: frames, envelope, decoders, model, the CA and
# the route service. Shared by the sidecar, the route daemon and the tests.
add_library(phi_adapter_thinq_core STATIC
    src/thinq_ca.cpp
    src/thinq_dishwasher.cpp
    src/thinq_envelope.cpp
    src/thinq_frame.cpp
    src/thinq_json.cpp
    src/thinq_model.cpp
    src/thinq_route.cpp
    src/thinq_schema.cpp
    src/thinq_settings.cpp
    src/thinq_washer.cpp
)
target_compile_features(phi_adapter_thinq_core PUBLIC cxx_std_20)
set_target_properties(phi_adapter_thinq_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(phi_adapter_thinq_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(phi_adapter_thinq_core
    PUBLIC phi::adapter-sdk nlohmann_json::nlohmann_json
    PRIVATE OpenSSL::SSL OpenSSL::Crypto)

add_executable(phi_adapter_thinq_ipc
    src/main.cpp
    src/thinq_instance.cpp
    src/thinq_mqtt.cpp
)
target_link_libraries(phi_adapter_thinq_ipc PRIVATE phi_adapter_thinq_core mosquitto::mosquitto)
set_target_properties(phi_adapter_thinq_ipc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters")

add_executable(phi-thinq-routed src/routed_main.cpp)
target_link_libraries(phi-thinq-routed PRIVATE phi_adapter_thinq_core OpenSSL::SSL OpenSSL::Crypto)

enable_testing()
# Frames, envelope, decoders and the model, without a broker.
add_executable(thinq_protocol_tests tests/thinq_protocol_tests.cpp)
target_link_libraries(thinq_protocol_tests PRIVATE phi_adapter_thinq_core)
add_test(NAME thinq_protocol_tests COMMAND thinq_protocol_tests)
set_tests_properties(thinq_protocol_tests PROPERTIES TIMEOUT 120)

# The CA and the route service: minting, signing a CSR, the four endpoints.
add_executable(thinq_route_tests tests/thinq_route_tests.cpp)
target_link_libraries(thinq_route_tests PRIVATE phi_adapter_thinq_core OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME thinq_route_tests COMMAND thinq_route_tests)
set_tests_properties(thinq_route_tests PROPERTIES TIMEOUT 120)

install(TARGETS phi_adapter_thinq_ipc
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters)
install(TARGETS phi-thinq-routed
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/phi)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/mosquitto-thinq.conf"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/dnsmasq-thinq.conf"
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples)
