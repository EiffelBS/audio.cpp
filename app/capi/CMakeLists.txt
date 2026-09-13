# audiocpp_engine: SHARED C API over engine_runtime, for in-process hosts.
#
# Deliberately a DLL (not a static lib): the vendored ggml stays
# symbol-isolated inside the module, so hosts that link their own ggml
# (whisper.cpp ports, minimaxmusic.cpp) do not collide with it. Only the
# C API is exported (audiocpp_engine.def).
#
# request.cpp comes from app/cli: build_request_from_json is the same
# parser the server uses for /v1/tasks/run, so request JSON behaves
# identically in-process and over HTTP. args.cpp holds the option-name
# helpers request.cpp uses.

add_library(audiocpp_engine SHARED
    audiocpp_capi.cpp
    ../../src/capi/audiocpp.cpp
    ../cli/request.cpp
    ../cli/args.cpp
)

# AUDIOCPP_ENGINE_EXPORTS gives the acpp_* facade its dllexport; AUDIOCPP_BUILD_DLL
# does the same for the upstream audiocpp_* facade (include/audiocpp.h, #530).
# On MSVC .def and dllexport are additive: the .def below keeps the 10 acpp_* and
# dllexport brings the 69 audiocpp_* (cJSON no longer leaks via CJSON_HIDE_SYMBOLS).
target_compile_definitions(audiocpp_engine PRIVATE
    AUDIOCPP_ENGINE_EXPORTS
    AUDIOCPP_BUILD_DLL
    AUDIOCPP_VERSION_STRING="${AUDIOCPP_VERSION}")
target_compile_features(audiocpp_engine PRIVATE cxx_std_20)

target_include_directories(audiocpp_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(audiocpp_engine PRIVATE engine_runtime)

if (MSVC)
    target_compile_options(audiocpp_engine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# Export surface: the .def keeps the 10 acpp_* entry points, dllexport
# annotations in the objects add the 69 audiocpp_* (MSVC unions both lists;
# cJSON is silenced via CJSON_HIDE_SYMBOLS so only these 79 leave the module).
target_sources(audiocpp_engine PRIVATE audiocpp_engine.def)

set_target_properties(audiocpp_engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# --- standalone smoke (also the host-integration proof) --------------------
add_executable(capi_smoke capi_smoke.cpp)
target_compile_features(capi_smoke PRIVATE cxx_std_20)
target_link_libraries(capi_smoke PRIVATE audiocpp_engine)
if (MSVC)
    target_compile_options(capi_smoke PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
