if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    message(FATAL_ERROR "SPARK_BUILD_LEVILAMINA requires Windows x64")
endif ()
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "SPARK_BUILD_LEVILAMINA requires clang-cl with the MSVC frontend")
endif ()

set(SPARK_LL_SDK_ROOT "" CACHE PATH
        "External LeviLamina 26.51.6 SDK root")
set(SPARK_LL_RUNTIME_DLL "" CACHE FILEPATH
        "External LeviLamina runtime DLL used to synthesize the import library")
set(SPARK_LL_RUNTIME_PDB "" CACHE FILEPATH
        "External LeviLamina runtime PDB recorded with the import inventory")
set(SPARK_LL_RUNTIME_DATA "" CACHE FILEPATH
        "External bedrock runtime data passed to prelink")
set(SPARK_LL_PRELINK "" CACHE FILEPATH
        "External pinned prelink 0.8.6 executable")
set(SPARK_LL_IMPORT_ALLOWLIST "${CMAKE_CURRENT_SOURCE_DIR}/tools/levilamina/spark-levilamina-imports.json" CACHE FILEPATH
        "Validated named LeviLamina exports admitted to the import library")
set(SPARK_LL_SYMBOLPROVIDER_SOURCE "" CACHE FILEPATH
        "External pinned SymbolProvider delay-load glue source")

set(_spark_ll_required_inputs
        SPARK_LL_SDK_ROOT
        SPARK_LL_RUNTIME_DLL
        SPARK_LL_RUNTIME_PDB
        SPARK_LL_RUNTIME_DATA
        SPARK_LL_PRELINK
        SPARK_LL_SYMBOLPROVIDER_SOURCE)
foreach (_spark_ll_input IN LISTS _spark_ll_required_inputs)
    if (NOT DEFINED ${_spark_ll_input} OR "${${_spark_ll_input}}" STREQUAL "")
        message(FATAL_ERROR
                "SPARK_BUILD_LEVILAMINA requires an explicit -D${_spark_ll_input}=... external build input")
    endif ()
endforeach ()

set(_spark_ll_required_paths
        "${SPARK_LL_SDK_ROOT}/levilamina/include/common/ll/api/Global.h"
        "${SPARK_LL_SDK_ROOT}/levilamina/include/common/ll/api/event/world/ServerLevelTickEvent.h"
        "${SPARK_LL_SDK_ROOT}/dependencies/entt/include/entt/entt.hpp"
        "${SPARK_LL_SDK_ROOT}/dependencies/expected-lite/include/nonstd/expected.hpp"
        "${SPARK_LL_SDK_ROOT}/dependencies/glm/include/glm/detail/type_vec2.inl"
        "${SPARK_LL_SDK_ROOT}/dependencies/fmt/include/fmt/format.h"
        "${SPARK_LL_SDK_ROOT}/dependencies/gsl/include/gsl/gsl"
        "${SPARK_LL_SDK_ROOT}/dependencies/parallel-hashmap/include/parallel_hashmap/phmap.h"
        "${SPARK_LL_RUNTIME_DLL}"
        "${SPARK_LL_RUNTIME_PDB}"
        "${SPARK_LL_RUNTIME_DATA}"
        "${SPARK_LL_PRELINK}"
        "${SPARK_LL_IMPORT_ALLOWLIST}"
        "${SPARK_LL_SYMBOLPROVIDER_SOURCE}")
foreach (_spark_ll_required IN LISTS _spark_ll_required_paths)
    if (NOT EXISTS "${_spark_ll_required}")
        message(FATAL_ERROR "LeviLamina SDK/runtime prerequisite is missing: ${_spark_ll_required}")
    endif ()
endforeach ()
file(SHA256 "${SPARK_LL_SDK_ROOT}/dependencies/expected-lite/include/nonstd/expected.hpp" _spark_ll_expected_lite_hash)
if (NOT _spark_ll_expected_lite_hash STREQUAL "14a2a36b32bcb66e1128c721e2ade36a90d9d9ad7207ea5ea81662d5d1f41cbf")
    message(FATAL_ERROR "LeviLamina SDK expected-lite header is not the pinned f339d2f73730f8fee4412f5e4938717866ecef48 commit")
endif ()
file(SHA256 "${SPARK_LL_SYMBOLPROVIDER_SOURCE}" _spark_ll_symbolprovider_hash)
if (NOT _spark_ll_symbolprovider_hash STREQUAL "7478d26ef21ea417383bf276deba540126410a7abb700402f7cedd79e2bb79fd")
    message(FATAL_ERROR "SymbolProvider source hash does not match pinned commit 6c93ec45c8455992ee726d92df60316c8e731c44")
endif ()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(SPARK_LL_DLLTOOL NAMES llvm-dlltool llvm-dlltool.exe
        HINTS "${CMAKE_CXX_COMPILER}" "${CMAKE_CXX_COMPILER_ID}")
if (NOT SPARK_LL_DLLTOOL)
    find_program(SPARK_LL_DLLTOOL NAMES llvm-dlltool llvm-dlltool.exe)
endif ()
if (NOT SPARK_LL_DLLTOOL)
    message(FATAL_ERROR "llvm-dlltool is required to synthesize the LL import library")
endif ()

set(_spark_ll_common_include "${SPARK_LL_SDK_ROOT}/levilamina/include/common")
set(_spark_ll_server_include "${SPARK_LL_SDK_ROOT}/levilamina/include/server")
set(_spark_ll_dependency_includes
        "${SPARK_LL_SDK_ROOT}/dependencies/entt/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/expected-lite/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/fmt/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/gsl/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/glm/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/leveldb/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/magic_enum/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/nlohmann_json/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/rapidjson/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/type_safe/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/pcg_cpp/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/pfr/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/concurrentqueue/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/stb/include"
        "${SPARK_LL_SDK_ROOT}/dependencies/stb/include/stb"
        "${SPARK_LL_SDK_ROOT}/dependencies/parallel-hashmap/include")

add_library(spark_levilamina_objects OBJECT
        src/platform/levilamina/spark_mod.cpp
        src/platform/levilamina/adapters.cpp
        src/platform/levilamina/bds/player_ping.cpp
        src/platform/levilamina/bds/pubsub.cpp
        src/platform/levilamina/bds/tick_duration.cpp
        src/platform/levilamina/bds/world_access.cpp
        src/platform/levilamina/memory_operators.cpp
        src/platform/levilamina/callback_state.cpp
        src/platform/levilamina/cleanup_deadline_guard.cpp
        src/platform/levilamina/command_lifecycle.cpp
        src/platform/levilamina/host_parser_provenance.cpp
        src/platform/levilamina/host_command_parameter.cpp
        src/platform/levilamina/world_callback_admission.cpp
        src/platform/levilamina/world_gauge_provider.cpp)
target_include_directories(spark_levilamina_objects PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina_objects PRIVATE
        LL_PLAT_S
        ENTT_PACKED_PAGE=128
        ENTT_SPARSE_PAGE=2048
        ENTT_NO_MIXIN
        FMT_HEADER_ONLY
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _ITERATOR_DEBUG_LEVEL=0)
set_target_properties(spark_levilamina_objects PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_objects PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()

add_library(spark_levilamina_application STATIC
        src/platform/levilamina/application_bridge.cpp)
target_include_directories(spark_levilamina_application PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(spark_levilamina_application PRIVATE spark_application)
set_target_properties(spark_levilamina_application PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_application PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()

add_library(spark_levilamina_symbolprovider OBJECT "${SPARK_LL_SYMBOLPROVIDER_SOURCE}")
target_compile_definitions(spark_levilamina_symbolprovider PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS)
set_target_properties(spark_levilamina_symbolprovider PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_symbolprovider PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()

set(_spark_ll_config_dir "${CMAKE_CURRENT_BINARY_DIR}/levilamina/$<CONFIG>")
set(_spark_ll_import_lib "${_spark_ll_config_dir}/LeviLamina.lib")
set(_spark_ll_import_def "${_spark_ll_config_dir}/LeviLamina.def")
set(_spark_ll_import_receipt "${_spark_ll_config_dir}/LeviLamina-imports.json")
add_custom_command(
        OUTPUT "${_spark_ll_import_lib}"
        BYPRODUCTS "${_spark_ll_import_def}" "${_spark_ll_import_receipt}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_spark_ll_config_dir}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/levilamina/generate_import_library.py"
                --dll "${SPARK_LL_RUNTIME_DLL}"
                --pdb "${SPARK_LL_RUNTIME_PDB}"
                --allowlist "${SPARK_LL_IMPORT_ALLOWLIST}"
                --output-def "${_spark_ll_import_def}"
                --output-lib "${_spark_ll_import_lib}"
                --receipt "${_spark_ll_import_receipt}"
                --dlltool "${SPARK_LL_DLLTOOL}"
        DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/levilamina/generate_import_library.py"
        "${SPARK_LL_IMPORT_ALLOWLIST}"
        "${SPARK_LL_DLLTOOL}"
        "${SPARK_LL_RUNTIME_DLL}"
        "${SPARK_LL_RUNTIME_PDB}"
        VERBATIM)
add_custom_target(spark_levilamina_import_library DEPENDS "${_spark_ll_import_lib}")

set(_spark_ll_prelink_dir "${CMAKE_CURRENT_BINARY_DIR}/levilamina/prelink/$<CONFIG>")
set(_spark_ll_prelink_lib "${_spark_ll_prelink_dir}/lib/bedrock_runtime_api.lib")
# Prelink 0.8.6's server contract matches LeviBuildScript 0.6.1:
#   prelink.exe server-windows-x64 <outdir> <bedrock_runtime_data> <objects...>
# It writes bedrock_runtime_api.lib below <outdir>/lib.
add_custom_command(
        OUTPUT "${_spark_ll_prelink_lib}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_spark_ll_prelink_dir}/lib"
        COMMAND "${SPARK_LL_PRELINK}" server-windows-x64 "${_spark_ll_prelink_dir}" "${SPARK_LL_RUNTIME_DATA}"
                $<TARGET_OBJECTS:spark_levilamina_objects>
                $<TARGET_OBJECTS:spark_levilamina_symbolprovider>
        DEPENDS spark_levilamina_objects spark_levilamina_symbolprovider
                $<TARGET_OBJECTS:spark_levilamina_objects>
                $<TARGET_OBJECTS:spark_levilamina_symbolprovider>
                "${SPARK_LL_RUNTIME_DATA}" "${SPARK_LL_PRELINK}"
        COMMAND_EXPAND_LISTS
        VERBATIM)
add_custom_target(spark_levilamina_prelink DEPENDS "${_spark_ll_prelink_lib}")

add_library(spark_levilamina SHARED
        $<TARGET_OBJECTS:spark_levilamina_objects>
        $<TARGET_OBJECTS:spark_levilamina_symbolprovider>)
add_dependencies(spark_levilamina spark_levilamina_import_library spark_levilamina_prelink
        spark_levilamina_symbolprovider)
set(_spark_ll_name "levilamina_spark")
set(_spark_ll_pdb "${_spark_ll_name}.pdb")
target_include_directories(spark_levilamina PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina PRIVATE LL_PLAT_S ENTT_PACKED_PAGE=128 ENTT_SPARSE_PAGE=2048 ENTT_NO_MIXIN)
target_link_libraries(spark_levilamina PRIVATE spark_levilamina_application "${_spark_ll_prelink_lib}" "${_spark_ll_import_lib}")
target_link_options(spark_levilamina PRIVATE
        "/DELAYLOAD:bedrock_runtime.dll"
        "/MAP:$<TARGET_FILE_DIR:spark_levilamina>/${_spark_ll_name}.map"
        "/MAPINFO:EXPORTS"
        "/PDB:$<TARGET_FILE_DIR:spark_levilamina>/${_spark_ll_pdb}")
set_target_properties(spark_levilamina PROPERTIES
        OUTPUT_NAME "${_spark_ll_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/spark"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/spark"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/spark"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/spark"
        PDB_NAME "${_spark_ll_name}"
        COMPILE_PDB_NAME "${_spark_ll_name}"
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(_spark_ll_manifest "${CMAKE_CURRENT_BINARY_DIR}/levilamina/$<CONFIG>/manifest.json")
file(GENERATE OUTPUT "${_spark_ll_manifest}" CONTENT
"{\n  \"name\": \"spark\",\n  \"entry\": \"${_spark_ll_name}.dll\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"type\": \"native\",\n  \"platform\": \"server\"\n}\n")
add_custom_command(TARGET spark_levilamina POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:spark_levilamina>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_spark_ll_manifest}" "$<TARGET_FILE_DIR:spark_levilamina>/manifest.json"
        VERBATIM)

enable_testing()
add_test(NAME spark_levilamina_import_generator
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/levilamina/generate_import_library.py" --self-test)

add_executable(spark_levilamina_callback_protocol_test
        tests/levilamina/callback_state_test.cpp
        src/platform/levilamina/callback_state.cpp)
target_include_directories(spark_levilamina_callback_protocol_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(spark_levilamina_callback_protocol_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_callback_protocol_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_callback_protocol_test COMMAND spark_levilamina_callback_protocol_test)
set_tests_properties(spark_levilamina_callback_protocol_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_world_gauge_state_test
        tests/levilamina/world_gauge_state_test.cpp)
target_include_directories(spark_levilamina_world_gauge_state_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(spark_levilamina_world_gauge_state_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_world_gauge_state_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_world_gauge_state_test COMMAND spark_levilamina_world_gauge_state_test)
set_tests_properties(spark_levilamina_world_gauge_state_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_world_callback_admission_test
        tests/levilamina/world_callback_admission_test.cpp
        src/platform/levilamina/world_callback_admission.cpp
        src/platform/levilamina/callback_state.cpp)
target_include_directories(spark_levilamina_world_callback_admission_test PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina_world_callback_admission_test PRIVATE
        LL_PLAT_S
        ENTT_PACKED_PAGE=128
        ENTT_SPARSE_PAGE=2048
        ENTT_NO_MIXIN
        FMT_HEADER_ONLY
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _ITERATOR_DEBUG_LEVEL=0)
set_target_properties(spark_levilamina_world_callback_admission_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_world_callback_admission_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_world_callback_admission_test COMMAND spark_levilamina_world_callback_admission_test)
set_tests_properties(spark_levilamina_world_callback_admission_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_world_connector_abi_test
        tests/levilamina/world_connector_abi_test.cpp)
target_include_directories(spark_levilamina_world_connector_abi_test PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina_world_connector_abi_test PRIVATE
        LL_PLAT_S
        ENTT_PACKED_PAGE=128
        ENTT_SPARSE_PAGE=2048
        ENTT_NO_MIXIN
        FMT_HEADER_ONLY
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _ITERATOR_DEBUG_LEVEL=0)
set_target_properties(spark_levilamina_world_connector_abi_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_world_connector_abi_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_world_connector_abi_test COMMAND spark_levilamina_world_connector_abi_test)
set_tests_properties(spark_levilamina_world_connector_abi_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_command_lifecycle_test
        tests/levilamina/command_lifecycle_test.cpp
        src/platform/levilamina/command_lifecycle.cpp)
target_include_directories(spark_levilamina_command_lifecycle_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(spark_levilamina_command_lifecycle_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_command_lifecycle_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_command_lifecycle_test COMMAND spark_levilamina_command_lifecycle_test)
set_tests_properties(spark_levilamina_command_lifecycle_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_shutdown_policy_test
        tests/levilamina/shutdown_policy_test.cpp
        src/platform/levilamina/cleanup_deadline_guard.cpp)
target_include_directories(spark_levilamina_shutdown_policy_test PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina_shutdown_policy_test PRIVATE LL_PLAT_S)
set_target_properties(spark_levilamina_shutdown_policy_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_shutdown_policy_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_shutdown_policy_test COMMAND spark_levilamina_shutdown_policy_test)
set_tests_properties(spark_levilamina_shutdown_policy_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_publication_boundary_test
        tests/levilamina/publication_boundary_test.cpp
        src/platform/levilamina/command_lifecycle.cpp
        src/platform/levilamina/cleanup_deadline_guard.cpp)
target_include_directories(spark_levilamina_publication_boundary_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(spark_levilamina_publication_boundary_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_publication_boundary_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_publication_boundary_test COMMAND spark_levilamina_publication_boundary_test)
set_tests_properties(spark_levilamina_publication_boundary_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_command_layout_test
        tests/levilamina/command_layout_test.cpp)
target_include_directories(spark_levilamina_command_layout_test PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes})
target_compile_definitions(spark_levilamina_command_layout_test PRIVATE
        LL_PLAT_S
        ENTT_PACKED_PAGE=128
        ENTT_SPARSE_PAGE=2048
        ENTT_NO_MIXIN
        FMT_HEADER_ONLY
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _ITERATOR_DEBUG_LEVEL=0)
set_target_properties(spark_levilamina_command_layout_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_command_layout_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_command_layout_test COMMAND spark_levilamina_command_layout_test)
set_tests_properties(spark_levilamina_command_layout_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_host_raw_parameter_test
        tests/levilamina/host_raw_parameter_test.cpp
        src/platform/levilamina/host_parser_provenance.cpp)
target_include_directories(spark_levilamina_host_raw_parameter_test PRIVATE
        "${_spark_ll_server_include}"
        "${_spark_ll_common_include}"
        ${_spark_ll_dependency_includes}
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(spark_levilamina_host_raw_parameter_test PRIVATE
        LL_PLAT_S
        ENTT_PACKED_PAGE=128
        ENTT_SPARSE_PAGE=2048
        ENTT_NO_MIXIN
        FMT_HEADER_ONLY
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _ITERATOR_DEBUG_LEVEL=0)
set_target_properties(spark_levilamina_host_raw_parameter_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_host_raw_parameter_test PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_test(NAME spark_levilamina_host_raw_parameter_test COMMAND spark_levilamina_host_raw_parameter_test)
set_tests_properties(spark_levilamina_host_raw_parameter_test PROPERTIES TIMEOUT 30)

add_executable(spark_levilamina_cleanup_deadline_test_child
        tests/levilamina/cleanup_deadline_test_child.cpp
        src/platform/levilamina/callback_state.cpp
        src/platform/levilamina/cleanup_deadline_guard.cpp)
target_include_directories(spark_levilamina_cleanup_deadline_test_child PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(spark_levilamina_cleanup_deadline_test_child PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_cleanup_deadline_test_child PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()

add_executable(spark_levilamina_cleanup_deadline_test_driver
        tests/levilamina/cleanup_deadline_test_driver.cpp)
set_target_properties(spark_levilamina_cleanup_deadline_test_driver PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|MSVC")
    target_compile_options(spark_levilamina_cleanup_deadline_test_driver PRIVATE /utf-8 /permissive- /EHsc /Zc:__cplusplus)
endif ()
add_dependencies(spark_levilamina_cleanup_deadline_test_driver spark_levilamina_cleanup_deadline_test_child)
add_test(NAME spark_levilamina_cleanup_deadline_test_driver COMMAND spark_levilamina_cleanup_deadline_test_driver)
set_tests_properties(spark_levilamina_cleanup_deadline_test_driver PROPERTIES TIMEOUT 30)

message(STATUS "LeviLamina 26.51 target enabled with isolated SDK includes: ${_spark_ll_server_include}; ${_spark_ll_common_include}")
