# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tmnt_turtles_in_time_reshelled VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where tools/codegen.ps1 and tools/package-linux.sh unpack the pinned SDK.
if(WIN32)
    set(TMNT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue/win-amd64")
else()
    set(TMNT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue/linux-amd64")
endif()
list(APPEND CMAKE_PREFIX_PATH "${TMNT_SDK_DIR}")

# Release binaries must not carry the build machine's folders (__FILE__ in log calls).
add_compile_options("$<$<CONFIG:Release>:-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.>")

include(generated/rexglue.cmake)

# TRG Launcher: the third_party/trg-launcher submodule, or a local checkout
# named by TMNT_TRG_LAUNCHER_DIR while the submodule is not checked out.
set(TMNT_TRG_LAUNCHER_DIR "$ENV{TMNT_TRG_LAUNCHER_DIR}" CACHE PATH
    "Local TRG Launcher checkout, used when third_party/trg-launcher is empty")

# Upstream TRG is immutable. Every build compiles a patched copy under the
# binary directory; the submodule checkout must stay at the pinned revision.
set(TMNT_TRG_LAUNCHER_REVISION "b384a28c712c9320eea8f9d50639dec49981b6c1")
set(TMNT_TRG_LAUNCHER_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches/trg-launcher")
set(TMNT_TRG_LAUNCHER_PATCH_STAMP_VERSION 1)
file(GLOB _trg_patch_names CONFIGURE_DEPENDS RELATIVE "${TMNT_TRG_LAUNCHER_PATCH_DIR}"
    "${TMNT_TRG_LAUNCHER_PATCH_DIR}/*.patch")
list(SORT _trg_patch_names)
# The glob dependency only tracks added or removed files; list the patches as
# configure dependencies too so editing one triggers a reconfigure.
foreach(_trg_patch_name IN LISTS _trg_patch_names)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${TMNT_TRG_LAUNCHER_PATCH_DIR}/${_trg_patch_name}")
endforeach()

find_package(Git QUIET)
if(NOT Git_FOUND)
    message(FATAL_ERROR "TRG Launcher: git is required to apply patches/trg-launcher")
endif()

# Copies the source into output_dir and applies the patches there. The stamp
# covers the source state and every patch, so the copy is rebuilt only when one
# of them changes. GIT_CEILING_DIRECTORIES keeps git apply from discovering the
# parent repository when the generated copy has no .git of its own.
function(tmnt_prepare_patched_trg_launcher source_dir output_dir require_pinned)
    set(_source_state "")
    if(EXISTS "${source_dir}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
                        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _head_result ERROR_QUIET)
        if(NOT _head_result EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: ${source_dir} is not a git checkout; run `git submodule update --init third_party/trg-launcher`")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" status --porcelain --untracked-files=normal
                        OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" diff --binary HEAD
                        OUTPUT_VARIABLE _diff)
        string(SHA256 _source_state "${_head}\n${_status}\n${_diff}")
        if(require_pinned)
            if(NOT _head STREQUAL TMNT_TRG_LAUNCHER_REVISION)
                message(FATAL_ERROR "TRG Launcher: ${source_dir} is at ${_head}, expected the pinned ${TMNT_TRG_LAUNCHER_REVISION}. Run `git submodule update --init third_party/trg-launcher`.")
            endif()
            if(NOT _status STREQUAL "")
                message(FATAL_ERROR "TRG Launcher: ${source_dir} has local changes; run `git -C third_party/trg-launcher checkout .` and configure again.")
            endif()
        endif()
    else()
        file(GLOB_RECURSE _source_files LIST_DIRECTORIES false RELATIVE "${source_dir}" "${source_dir}/*")
        list(SORT _source_files)
        foreach(_file IN LISTS _source_files)
            if(_file MATCHES "(^|/)\\.git(/|$)")
                continue()
            endif()
            file(SHA256 "${source_dir}/${_file}" _file_hash)
            string(APPEND _source_state "${_file}\n${_file_hash}\n")
        endforeach()
        string(SHA256 _source_state "${_source_state}")
    endif()

    set(_fingerprint "${TMNT_TRG_LAUNCHER_PATCH_STAMP_VERSION}\n${source_dir}\n${_source_state}\n")
    foreach(_patch_name IN LISTS _trg_patch_names)
        file(SHA256 "${TMNT_TRG_LAUNCHER_PATCH_DIR}/${_patch_name}" _patch_hash)
        string(APPEND _fingerprint "${_patch_name}\n${_patch_hash}\n")
    endforeach()
    string(SHA256 _fingerprint "${_fingerprint}")

    set(_stamp "${output_dir}/.tmnt-trg-patch-stamp")
    if(EXISTS "${_stamp}" AND EXISTS "${output_dir}/CMakeLists.txt")
        file(READ "${_stamp}" _stamp_value)
        if(_stamp_value STREQUAL _fingerprint)
            return()
        endif()
    endif()

    message(STATUS "TRG Launcher: preparing patched copy in ${output_dir}")
    file(REMOVE_RECURSE "${output_dir}")
    file(MAKE_DIRECTORY "${output_dir}")
    file(COPY "${source_dir}/" DESTINATION "${output_dir}" PATTERN ".git" EXCLUDE)

    get_filename_component(_ceiling "${output_dir}" DIRECTORY)
    foreach(_patch_name IN LISTS _trg_patch_names)
        set(_patch "${TMNT_TRG_LAUNCHER_PATCH_DIR}/${_patch_name}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                        "${GIT_EXECUTABLE}" -C "${output_dir}" apply --check "${_patch}"
                        RESULT_VARIABLE _can_apply ERROR_VARIABLE _apply_error)
        if(NOT _can_apply EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: ${_patch} does not apply to ${source_dir} (${_apply_error}); the patch and the source revision are out of sync.")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${_ceiling}"
                        "${GIT_EXECUTABLE}" -C "${output_dir}" apply "${_patch}"
                        RESULT_VARIABLE _apply_result)
        if(NOT _apply_result EQUAL 0)
            message(FATAL_ERROR "TRG Launcher: failed to apply ${_patch}")
        endif()
    endforeach()
    file(WRITE "${_stamp}" "${_fingerprint}")
endfunction()

set(_trg_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/trg-launcher")
if(EXISTS "${_trg_source_dir}/CMakeLists.txt")
    set(_trg_require_pinned TRUE)
else()
    set(_trg_source_dir "${TMNT_TRG_LAUNCHER_DIR}")
    set(_trg_require_pinned FALSE)
endif()
if(NOT EXISTS "${_trg_source_dir}/CMakeLists.txt")
    message(FATAL_ERROR "TRG Launcher not found: run `git submodule update --init third_party/trg-launcher`")
endif()

set(_trg_patched_dir "${CMAKE_BINARY_DIR}/trg-launcher-patched")
tmnt_prepare_patched_trg_launcher("${_trg_source_dir}" "${_trg_patched_dir}" ${_trg_require_pinned})

file(READ "${_trg_patched_dir}/src/launcher.cpp" _trg_launcher_source)
string(FIND "${_trg_launcher_source}" "void Launcher::HandleHotkeys() {" _hotkeys_pos)
string(SUBSTRING "${_trg_launcher_source}" ${_hotkeys_pos} -1 _hotkeys_source)
string(FIND "${_hotkeys_source}" "ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)" _ctrl_s_pos)
string(FIND "${_hotkeys_source}" "io.WantTextInput) return;" _text_guard_pos)
if(_hotkeys_pos EQUAL -1 OR _ctrl_s_pos EQUAL -1 OR _text_guard_pos EQUAL -1 OR _ctrl_s_pos GREATER _text_guard_pos)
    message(FATAL_ERROR "TRG Launcher: the staged HandleHotkeys does not handle Ctrl+S before the text-input guard (${_trg_patched_dir}/src/launcher.cpp)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_trg_patched_dir}/src/launcher.cpp")

# Reconfigure when the patches, the upstream checkout or its HEAD change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_trg_source_dir}/CMakeLists.txt")
if(EXISTS "${_trg_source_dir}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_trg_source_dir}" rev-parse --git-path HEAD
                    OUTPUT_VARIABLE _trg_head_path OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_trg_head_path)
        get_filename_component(_trg_head_path "${_trg_head_path}" ABSOLUTE BASE_DIR "${_trg_source_dir}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_trg_head_path}")
    endif()
endif()

# The launcher draws inside the runtime's own Dear ImGui frame, so it must use
# the ImGui compiled into rexruntime (one context), never a downloaded copy.
add_library(tmnt_imgui INTERFACE)
target_link_libraries(tmnt_imgui INTERFACE rex::runtime)
set(TRG_LAUNCHER_IMGUI_TARGET tmnt_imgui CACHE STRING "" FORCE)
set(TRG_LAUNCHER_STANDALONE OFF CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(TRG_LAUNCHER_FETCH_DEPS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_trg_patched_dir}" trg-launcher)
# Written for C++17: in C++20 and later u8string() is a different type.
set_target_properties(trg_launcher PROPERTIES CXX_STANDARD 17)
# TRG defines this only for cl.exe; clang++ needs it too for the MSVC CRT.
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
get_directory_property(TMNT_TRG_VERSION DIRECTORY "${_trg_patched_dir}" DEFINITION PROJECT_VERSION)

set(TMNT_SOURCES
    src/main.cpp
    src/achievement_alias.cpp
    src/achievement_menu.cpp
    src/achievements_overlay.cpp
    src/active_log.cpp
    src/crash_report.cpp
    src/diagnostics.cpp
    src/frame_counter.cpp
    src/frame_limiter.cpp
    src/frame_wait.cpp
    src/game_files.cpp
    src/graphics_adapters.cpp
    src/guest_functions.cpp
    src/imgui_gamepad.cpp
    src/input_summary.cpp
    src/launcher.cpp
    src/launcher_achievements.cpp
    src/legacy_settings.cpp
    src/live_fix.cpp
    src/live_stats.cpp
    src/local_stats.cpp
    src/log_filter.cpp
    src/log_path.cpp
    src/modal_dialog.cpp
    src/notice_dialog.cpp
    src/platform.cpp
    src/port_version.cpp
    src/session_log.cpp
    src/settings.cpp
    src/stfs.cpp
    src/system_info.cpp
    src/update_config.cpp
    src/updater/github_release.cpp
    src/updater/json.cpp
    src/updater/sha256.cpp
    src/updater/third_party/miniz/miniz.c
    src/updater/update_controller.cpp
    src/updater/update_job.cpp
    src/updater/update_paths.cpp
    src/updater/update_platform.cpp
    src/updater/update_ui.cpp
    src/updater/update_zip.cpp
    src/updater/version.cpp
    src/user_data.cpp
    src/window_host.cpp
)
set_source_files_properties(src/updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
if(WIN32)
    # Resource 1 is the first icon in the executable: Explorer, and SDL's window class, use it.
    enable_language(RC)
    set(TMNT_APP_RC "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/tmnt_turtles_in_time_reshelled.rc")
    set_source_files_properties("${TMNT_APP_RC}" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/app-icon.ico")
    list(APPEND TMNT_SOURCES "${TMNT_APP_RC}")
    add_executable(tmnt_turtles_in_time_reshelled WIN32 ${TMNT_SOURCES})
    set(TMNT_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/tmnt_turtles_in_time_reshelled.manifest")
    target_link_options(tmnt_turtles_in_time_reshelled PRIVATE "LINKER:/MANIFESTINPUT:${TMNT_APP_MANIFEST}")
    set_property(TARGET tmnt_turtles_in_time_reshelled APPEND PROPERTY LINK_DEPENDS "${TMNT_APP_MANIFEST}")
else()
    add_executable(tmnt_turtles_in_time_reshelled ${TMNT_SOURCES})
endif()

rexglue_setup_target(tmnt_turtles_in_time_reshelled GPU_PLUGINS xenos)
target_link_libraries(tmnt_turtles_in_time_reshelled PRIVATE trg::launcher)
if(WIN32)
    target_link_libraries(tmnt_turtles_in_time_reshelled PRIVATE shcore synchronization dxgi shell32)
    # The SDK stages its DLLs only when the program links, so a rebuilt runtime or GPU plugin has to
    # make it link again; tools/build.ps1 also checks the staged copies.
    foreach(_sdk_dll rexruntime rexruntimed rexruntimerd rexgpu-xenos rexgpu-xenosd rexgpu-xenosrd)
        if(EXISTS "${TMNT_SDK_DIR}/bin/${_sdk_dll}.dll")
            set_property(TARGET tmnt_turtles_in_time_reshelled APPEND PROPERTY LINK_DEPENDS
                "${TMNT_SDK_DIR}/bin/${_sdk_dll}.dll")
        endif()
    endforeach()
else()
    # The SDK stages the runtime beside the program only on Windows and macOS; the RPATH is $ORIGIN.
    add_custom_command(TARGET tmnt_turtles_in_time_reshelled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:rex::runtime>"
            "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>"
        VERBATIM
    )
    # The post-build copies (this one and the SDK's GPU plugin copy) only run when the program links, so a
    # rebuilt runtime or GPU plugin has to make it link again; tools/package-linux.sh also checks the copies.
    foreach(_sdk_lib rexruntime rexruntimed rexruntimerd rexgpu-xenos rexgpu-xenosd rexgpu-xenosrd)
        if(EXISTS "${TMNT_SDK_DIR}/lib/lib${_sdk_lib}.so")
            set_property(TARGET tmnt_turtles_in_time_reshelled APPEND PROPERTY LINK_DEPENDS
                "${TMNT_SDK_DIR}/lib/lib${_sdk_lib}.so")
        endif()
    endforeach()
endif()
# Every build checks that tools/rexglue holds the runtime and GPU plugin built from this branch's patches, and that
# the copies next to the program are those; tools/build.ps1 and tools/build.sh rebuild them when they are not.
if(WIN32)
    # Windows PowerShell has a fixed path, unlike a Store pwsh; the module path a parent pwsh sets breaks it.
    find_program(TMNT_WINDOWS_POWERSHELL NAMES powershell REQUIRED)
    set(TMNT_REXGLUE_CHECK "${CMAKE_COMMAND}" -E env --unset=PSModulePath
        "${TMNT_WINDOWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-patched-rexruntime.ps1" -Check -Config $<CONFIG>)
    set(TMNT_REXGLUE_STAGED -Staged)
else()
    find_program(TMNT_BASH bash REQUIRED)
    set(TMNT_REXGLUE_CHECK "${TMNT_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-patched-rexruntime.sh"
        --check "${TMNT_SDK_DIR}" --config $<CONFIG>)
    set(TMNT_REXGLUE_STAGED --staged)
endif()
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo|Release)$")
    message(FATAL_ERROR "Set CMAKE_BUILD_TYPE to Debug, RelWithDebInfo or Release (the presets and tools/build.ps1 "
        "do): the ReXGlue runtime and GPU plugin are checked per configuration")
endif()
add_custom_target(tmnt_rexglue_check COMMAND ${TMNT_REXGLUE_CHECK} VERBATIM)
add_dependencies(tmnt_turtles_in_time_reshelled tmnt_rexglue_check)
add_custom_target(tmnt_rexglue_staging ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:rex::runtime>" "$<TARGET_FILE:rex::gpu-xenos>"
        "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>"
    COMMAND ${TMNT_REXGLUE_CHECK} ${TMNT_REXGLUE_STAGED} "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>"
    VERBATIM
)
add_dependencies(tmnt_rexglue_staging tmnt_turtles_in_time_reshelled)
# ThinLTO in Release and RelWithDebInfo (the default of tools/build.ps1); the runtime and GPU plugin are
# prebuilt. Debug compiles at -O0, which marks every function optnone, so LTO would optimize nothing there.
function(tmnt_enable_thinlto target)
    set(_lto "$<CONFIG:Release,RelWithDebInfo>")
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        # lld-link runs the LTO backend at -O2 unless told otherwise; Release compiles at -O3.
        set(_link -flto=thin "$<$<CONFIG:Release>:-Wl,/opt:lldlto=3>"
            "-Wl,/lldltocache:${CMAKE_BINARY_DIR}/lto-cache")
    elseif(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_link -flto=thin -fuse-ld=lld "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
    else()
        return()
    endif()
    target_compile_options(${target} PRIVATE "$<$<AND:${_lto},$<COMPILE_LANGUAGE:CXX>>:-flto=thin>")
    get_target_property(_type ${target} TYPE)
    if(NOT _type STREQUAL "OBJECT_LIBRARY")
        target_link_options(${target} PRIVATE "$<${_lto}:${_link}>")
    endif()
endfunction()
tmnt_enable_thinlto(tmnt_turtles_in_time_reshelled)
if(TARGET tmnt_turtles_in_time_reshelled_recomp)
    tmnt_enable_thinlto(tmnt_turtles_in_time_reshelled_recomp)
endif()
if(TARGET tmnt_turtles_in_time_reshelled_TMNT_r)
    target_sources(tmnt_turtles_in_time_reshelled_TMNT_r PRIVATE src/live_hooks_tmnt_r.cpp)
    target_include_directories(tmnt_turtles_in_time_reshelled_TMNT_r PRIVATE src)
    tmnt_enable_thinlto(tmnt_turtles_in_time_reshelled_TMNT_r)
endif()

# Release packages ship the MSYS2 CLANG64 build of these sources (tools/build-updater.ps1).
if(WIN32)
    add_executable(trg-updater WIN32
        src/updater/helper_main.cpp
        src/updater/install_transaction.cpp
        src/updater/sha256.cpp
        src/updater/update_job.cpp
        src/updater/update_paths.cpp
        src/updater/update_platform.cpp
    )
    target_include_directories(trg-updater PRIVATE src)
    target_compile_definitions(trg-updater PRIVATE TMNT_UPDATER_HELPER_BUILD=1)
    target_link_libraries(trg-updater PRIVATE shell32)
    target_link_options(trg-updater PRIVATE -static)
    set_property(TARGET trg-updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(trg-updater PROPERTIES OUTPUT_NAME "trg-updater")
endif()
set(TMNT_VERSION_DEFINITIONS
    TMNT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    TMNT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    TMNT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_compile_definitions(tmnt_turtles_in_time_reshelled PRIVATE
    ${TMNT_VERSION_DEFINITIONS}
    TMNT_TRG_VERSION="${TMNT_TRG_VERSION}"
    TMNT_BUILD_CONFIG="$<CONFIG>")

# The runtime reads its cvars from <exe dir>/tmnt_turtles_in_time_reshelled.toml.
add_custom_command(TARGET tmnt_turtles_in_time_reshelled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/tmnt_turtles_in_time_reshelled.toml"
        "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>"
    VERBATIM
)

add_custom_command(TARGET tmnt_turtles_in_time_reshelled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>/assets/launcher"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header-art.png"
        "$<TARGET_FILE_DIR:tmnt_turtles_in_time_reshelled>/assets/launcher/header-art.png"
    VERBATIM
)

find_package(Threads REQUIRED)

# Installer tests, not part of the default build: cmake --build <dir> --target tmnt_installer_tests
add_executable(tmnt_installer_tests EXCLUDE_FROM_ALL
    tests/installer_tests.cpp
    src/game_files.cpp
    src/stfs.cpp
)
target_include_directories(tmnt_installer_tests PRIVATE src)
target_link_libraries(tmnt_installer_tests PRIVATE Threads::Threads)


# Frame counter tests, not part of the default build: cmake --build <dir> --target tmnt_frame_counter_tests
add_executable(tmnt_frame_counter_tests EXCLUDE_FROM_ALL tests/frame_counter_tests.cpp)
target_include_directories(tmnt_frame_counter_tests PRIVATE src)
target_compile_definitions(tmnt_frame_counter_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Guest frame limiter tests, not part of the default build:
# cmake --build <dir> --target tmnt_frame_limiter_tests
add_executable(tmnt_frame_limiter_tests EXCLUDE_FROM_ALL tests/frame_limiter_tests.cpp src/frame_wait.cpp)
target_include_directories(tmnt_frame_limiter_tests PRIVATE src)
target_link_libraries(tmnt_frame_limiter_tests PRIVATE Threads::Threads)
target_compile_definitions(tmnt_frame_limiter_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")





# GPU patch tests (register ranges, host thread affinity, scissor compare), not part of the default build:
# cmake --build <dir> --target tmnt_gpu_perf_patches_tests
add_executable(tmnt_gpu_perf_patches_tests EXCLUDE_FROM_ALL tests/gpu_perf_patches_tests.cpp)
target_include_directories(tmnt_gpu_perf_patches_tests PRIVATE src)
target_include_directories(tmnt_gpu_perf_patches_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include")
target_compile_definitions(tmnt_gpu_perf_patches_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")



# Keyboard remapping tests, not part of the default build: cmake --build <dir> --target tmnt_keyboard_tests
add_executable(tmnt_keyboard_tests EXCLUDE_FROM_ALL tests/keyboard_tests.cpp)
target_include_directories(tmnt_keyboard_tests PRIVATE src)
target_include_directories(tmnt_keyboard_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include" "${_trg_patched_dir}/include")
target_compile_definitions(tmnt_keyboard_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")


# Player names tests, not part of the default build: cmake --build <dir> --target tmnt_player_names_tests
add_executable(tmnt_player_names_tests EXCLUDE_FROM_ALL tests/player_names_tests.cpp)
target_include_directories(tmnt_player_names_tests PRIVATE src)
target_compile_definitions(tmnt_player_names_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Multisampling setting tests, not part of the default build: cmake --build <dir> --target tmnt_multisampling_tests
add_executable(tmnt_multisampling_tests EXCLUDE_FROM_ALL tests/multisampling_tests.cpp)
target_include_directories(tmnt_multisampling_tests PRIVATE src)
target_compile_definitions(tmnt_multisampling_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Aspect Ratio tests, not part of the default build: cmake --build <dir> --target tmnt_aspect_ratio_tests
add_executable(tmnt_aspect_ratio_tests EXCLUDE_FROM_ALL tests/aspect_ratio_tests.cpp)
target_include_directories(tmnt_aspect_ratio_tests PRIVATE src)
target_compile_definitions(tmnt_aspect_ratio_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Internal render resolution tests, not part of the default build:
# cmake --build <dir> --target tmnt_internal_render_scale_tests
add_executable(tmnt_internal_render_scale_tests EXCLUDE_FROM_ALL tests/internal_render_scale_tests.cpp)
target_include_directories(tmnt_internal_render_scale_tests PRIVATE src)
target_link_libraries(tmnt_internal_render_scale_tests PRIVATE trg::launcher)
target_compile_definitions(tmnt_internal_render_scale_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Lock-free guest clock tests, not part of the default build:
# cmake --build <dir> --target tmnt_guest_clock_tests
add_executable(tmnt_guest_clock_tests EXCLUDE_FROM_ALL tests/guest_clock_tests.cpp)
target_include_directories(tmnt_guest_clock_tests PRIVATE src)
target_compile_definitions(tmnt_guest_clock_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tmnt_guest_clock_tests PRIVATE Threads::Threads)

# D3D12 transfer short-circuit tests, not part of the default build:
# cmake --build <dir> --target tmnt_d3d12_transfer_short_circuit_tests
add_executable(tmnt_d3d12_transfer_short_circuit_tests EXCLUDE_FROM_ALL tests/d3d12_transfer_short_circuit_tests.cpp)
target_include_directories(tmnt_d3d12_transfer_short_circuit_tests PRIVATE src)
target_compile_definitions(tmnt_d3d12_transfer_short_circuit_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# D3D12 sampler memoization tests, not part of the default build:
# cmake --build <dir> --target tmnt_d3d12_sampler_memo_tests
add_executable(tmnt_d3d12_sampler_memo_tests EXCLUDE_FROM_ALL tests/d3d12_sampler_memo_tests.cpp)
target_include_directories(tmnt_d3d12_sampler_memo_tests PRIVATE src)
target_compile_definitions(tmnt_d3d12_sampler_memo_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Shared memory fast path tests, not part of the default build:
# cmake --build <dir> --target tmnt_shared_memory_fast_path_tests
add_executable(tmnt_shared_memory_fast_path_tests EXCLUDE_FROM_ALL tests/shared_memory_fast_path_tests.cpp)
target_include_directories(tmnt_shared_memory_fast_path_tests PRIVATE src)
target_compile_definitions(tmnt_shared_memory_fast_path_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tmnt_shared_memory_fast_path_tests PRIVATE Threads::Threads)

# GPU draw state memoization tests, not part of the default build:
# cmake --build <dir> --target tmnt_gpu_draw_state_memo_tests
add_executable(tmnt_gpu_draw_state_memo_tests EXCLUDE_FROM_ALL tests/gpu_draw_state_memo_tests.cpp)
target_include_directories(tmnt_gpu_draw_state_memo_tests PRIVATE src)
target_compile_definitions(tmnt_gpu_draw_state_memo_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# D3D12 asynchronous submission tests, not part of the default build:
# cmake --build <dir> --target tmnt_d3d12_async_submission_tests
add_executable(tmnt_d3d12_async_submission_tests EXCLUDE_FROM_ALL tests/d3d12_async_submission_tests.cpp)
target_include_directories(tmnt_d3d12_async_submission_tests PRIVATE src)
target_compile_definitions(tmnt_d3d12_async_submission_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tmnt_d3d12_async_submission_tests PRIVATE Threads::Threads)

# Graphics card setting tests, not part of the default build: cmake --build <dir> --target tmnt_graphics_adapter_tests
add_executable(tmnt_graphics_adapter_tests EXCLUDE_FROM_ALL tests/graphics_adapter_tests.cpp src/graphics_adapters.cpp)
target_include_directories(tmnt_graphics_adapter_tests PRIVATE src)
target_link_libraries(tmnt_graphics_adapter_tests PRIVATE trg::launcher)
if(WIN32)
    target_link_libraries(tmnt_graphics_adapter_tests PRIVATE dxgi d3d12)
endif()
target_compile_definitions(tmnt_graphics_adapter_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Relaunch tests, not part of the default build: cmake --build <dir> --target tmnt_relaunch_tests
add_executable(tmnt_relaunch_tests EXCLUDE_FROM_ALL tests/relaunch_tests.cpp)
target_include_directories(tmnt_relaunch_tests PRIVATE src)
target_compile_definitions(tmnt_relaunch_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Cursor last-input and window title tests, not part of the default build:
# cmake --build <dir> --target tmnt_window_host_tests
add_executable(tmnt_window_host_tests EXCLUDE_FROM_ALL tests/window_host_tests.cpp)
target_include_directories(tmnt_window_host_tests PRIVATE src)
target_include_directories(tmnt_window_host_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include")
target_compile_definitions(tmnt_window_host_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Log folder path tests, not part of the default build:
# cmake --build <dir> --target tmnt_log_path_tests
add_executable(tmnt_log_path_tests EXCLUDE_FROM_ALL tests/log_path_tests.cpp src/log_path.cpp)
target_include_directories(tmnt_log_path_tests PRIVATE src)

# Per-monitor DPI helper tests, not part of the default build:
# cmake --build <dir> --target tmnt_dpi_tests
add_executable(tmnt_dpi_tests EXCLUDE_FROM_ALL tests/dpi_tests.cpp)
target_include_directories(tmnt_dpi_tests PRIVATE src)

# Launcher window placement tests, not part of the default build:
# cmake --build <dir> --target tmnt_window_placement_tests
add_executable(tmnt_window_placement_tests EXCLUDE_FROM_ALL tests/window_placement_tests.cpp)
target_include_directories(tmnt_window_placement_tests PRIVATE src)

# Launcher quit behaviour tests (Escape must not quit), not part of the default build:
# cmake --build <dir> --target tmnt_launcher_quit_tests
add_executable(tmnt_launcher_quit_tests EXCLUDE_FROM_ALL tests/launcher_quit_tests.cpp)
target_include_directories(tmnt_launcher_quit_tests PRIVATE src)
target_compile_definitions(tmnt_launcher_quit_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Window-close lifecycle policy tests, not part of the default build:
# cmake --build <dir> --target tmnt_lifecycle_tests
add_executable(tmnt_lifecycle_tests EXCLUDE_FROM_ALL tests/lifecycle_tests.cpp)
target_include_directories(tmnt_lifecycle_tests PRIVATE src)
target_compile_definitions(tmnt_lifecycle_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Port version tests, not part of the default build: cmake --build <dir> --target tmnt_port_version_tests
add_executable(tmnt_port_version_tests EXCLUDE_FROM_ALL tests/port_version_tests.cpp src/port_version.cpp)
target_include_directories(tmnt_port_version_tests PRIVATE src)
target_compile_definitions(tmnt_port_version_tests PRIVATE
    ${TMNT_VERSION_DEFINITIONS}
    TMNT_EXPECTED_PORT_VERSION="${PROJECT_VERSION}")

# Startup diagnostic formatting tests, not part of the default build:
# cmake --build <dir> --target tmnt_diagnostics_tests
add_executable(tmnt_diagnostics_tests EXCLUDE_FROM_ALL
    tests/diagnostics_tests.cpp
    src/diagnostics.cpp
)
target_include_directories(tmnt_diagnostics_tests PRIVATE src)

# Single-log crash report tests, not part of the default build:
# cmake --build <dir> --target tmnt_crash_report_tests
add_executable(tmnt_crash_report_tests EXCLUDE_FROM_ALL
    tests/crash_report_tests.cpp
    src/crash_report.cpp
)
target_include_directories(tmnt_crash_report_tests PRIVATE src)
target_compile_definitions(tmnt_crash_report_tests PRIVATE TMNT_BUILD_CONFIG="RelWithDebInfo")
if(WIN32)
    target_link_libraries(tmnt_crash_report_tests PRIVATE shell32)
endif()

# Active log file tests, not part of the default build:
# cmake --build <dir> --target tmnt_active_log_tests
add_executable(tmnt_active_log_tests EXCLUDE_FROM_ALL
    tests/active_log_tests.cpp
    src/active_log.cpp
    src/log_filter.cpp
)
target_include_directories(tmnt_active_log_tests PRIVATE src)
target_include_directories(tmnt_active_log_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include")
target_link_libraries(tmnt_active_log_tests PRIVATE rex::runtime)
add_custom_command(TARGET tmnt_active_log_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>"
        "$<TARGET_FILE_DIR:tmnt_active_log_tests>"
    VERBATIM
)
if(NOT WIN32)
    set_target_properties(tmnt_active_log_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

# Detailed logging setting and Minimal log filter tests, not part of the default build:
# cmake --build <dir> --target tmnt_log_filter_tests
add_executable(tmnt_log_filter_tests EXCLUDE_FROM_ALL
    tests/log_filter_tests.cpp
    src/active_log.cpp
    src/crash_report.cpp
    src/log_filter.cpp
    src/session_log.cpp
)
target_include_directories(tmnt_log_filter_tests PRIVATE src)
target_include_directories(tmnt_log_filter_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include")
target_compile_definitions(tmnt_log_filter_tests PRIVATE
    TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
    TMNT_BUILD_CONFIG="RelWithDebInfo")
target_link_libraries(tmnt_log_filter_tests PRIVATE rex::runtime)
if(WIN32)
    target_link_libraries(tmnt_log_filter_tests PRIVATE shell32)
endif()
add_custom_command(TARGET tmnt_log_filter_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>"
        "$<TARGET_FILE_DIR:tmnt_log_filter_tests>"
    VERBATIM
)
if(NOT WIN32)
    set_target_properties(tmnt_log_filter_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()


# Achievements menu gate tests (B close consumes input, scroll, hold state),
# not part of the default build: cmake --build <dir> --target tmnt_achievement_menu_tests
add_executable(tmnt_achievement_menu_tests EXCLUDE_FROM_ALL tests/achievement_menu_tests.cpp)
target_include_directories(tmnt_achievement_menu_tests PRIVATE src)

# Offline leaderboard tests (local stats file, guest results buffers, enumerators),
# not part of the default build: cmake --build <dir> --target tmnt_live_tests
add_executable(tmnt_live_tests EXCLUDE_FROM_ALL
    tests/live_tests.cpp
    src/live_stats.cpp
    src/local_stats.cpp
)
target_include_directories(tmnt_live_tests PRIVATE src)
target_compile_definitions(tmnt_live_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# User data tests (portable root and migration), not part of the default build:
# cmake --build <dir> --target tmnt_user_data_tests
add_executable(tmnt_user_data_tests EXCLUDE_FROM_ALL
    tests/user_data_tests.cpp
    src/user_data.cpp
    src/local_stats.cpp
)
target_include_directories(tmnt_user_data_tests PRIVATE src)
target_include_directories(tmnt_user_data_tests SYSTEM PRIVATE "${TMNT_SDK_DIR}/include" "${_trg_patched_dir}/include")

# Updater tests (versions, release JSON, SHA-256, ZIP safety, staging and rollback), not part of the default build:
# cmake --build <dir> --target tmnt_update_tests
add_executable(tmnt_update_tests EXCLUDE_FROM_ALL
    tests/update_tests.cpp
    src/updater/github_release.cpp
    src/updater/install_transaction.cpp
    src/updater/json.cpp
    src/updater/sha256.cpp
    src/updater/update_job.cpp
    src/updater/update_paths.cpp
    src/updater/update_platform.cpp
    src/updater/update_zip.cpp
    src/updater/version.cpp
    src/updater/third_party/miniz/miniz.c
)
target_include_directories(tmnt_update_tests PRIVATE src)
if(WIN32)
    target_link_libraries(tmnt_update_tests PRIVATE shell32)
endif()
target_compile_definitions(tmnt_update_tests PRIVATE TMNT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src" TMNT_UPDATER_HELPER_BUILD=1)

# The real trg-updater.exe run from a temporary folder, not part of the default build:
# cmake --build <dir> --target tmnt_update_helper_tests, then run it, optionally with the path of another trg-updater.exe
if(WIN32)
    add_executable(tmnt_update_helper_tests EXCLUDE_FROM_ALL tests/update_helper_tests.cpp)
    # The test is also the program the helper restarts, which the helper starts without a console.
    set_target_properties(tmnt_update_helper_tests PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(tmnt_update_helper_tests PRIVATE "-Wl,/entry:mainCRTStartup")
    target_link_libraries(tmnt_update_helper_tests PRIVATE shell32)
    target_compile_definitions(tmnt_update_helper_tests PRIVATE TMNT_UPDATER_EXE="$<TARGET_FILE:trg-updater>")
    add_dependencies(tmnt_update_helper_tests trg-updater)
endif()

# The tests that compile game code are built like the game.
foreach(_test installer keyboard frame_counter multisampling internal_render_scale guest_clock d3d12_transfer_short_circuit d3d12_sampler_memo d3d12_async_submission graphics_adapter window_host window_placement launcher_quit lifecycle user_data update)
    tmnt_enable_thinlto(tmnt_${_test}_tests)
endforeach()

# Allocator stress test, not part of the default build: cmake --build <dir> --target tmnt_physical_heap_stress
add_executable(tmnt_physical_heap_stress EXCLUDE_FROM_ALL tests/physical_heap_stress.cpp)
target_link_libraries(tmnt_physical_heap_stress PRIVATE rex::runtime)
add_custom_command(TARGET tmnt_physical_heap_stress POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>"
        "$<TARGET_FILE_DIR:tmnt_physical_heap_stress>"
    VERBATIM
)
if(NOT WIN32)
    set_target_properties(tmnt_physical_heap_stress PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

# Exit Game shutdown order test, not part of the default build: cmake --build <dir> --target tmnt_shutdown_order_tests
add_executable(tmnt_shutdown_order_tests EXCLUDE_FROM_ALL tests/shutdown_order_tests.cpp)
target_link_libraries(tmnt_shutdown_order_tests PRIVATE rex::runtime)
add_custom_command(TARGET tmnt_shutdown_order_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>"
        "$<TARGET_FILE_DIR:tmnt_shutdown_order_tests>"
    VERBATIM
)
if(NOT WIN32)
    set_target_properties(tmnt_shutdown_order_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

# Guest thread shutdown test, not part of the default build: cmake --build <dir> --target tmnt_title_thread_shutdown_tests
add_executable(tmnt_title_thread_shutdown_tests EXCLUDE_FROM_ALL tests/title_thread_shutdown_tests.cpp)
target_link_libraries(tmnt_title_thread_shutdown_tests PRIVATE rex::runtime)
add_custom_command(TARGET tmnt_title_thread_shutdown_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:rex::runtime>"
        "$<TARGET_FILE_DIR:tmnt_title_thread_shutdown_tests>"
    VERBATIM
)
if(NOT WIN32)
    set_target_properties(tmnt_title_thread_shutdown_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

# RenderDoc detection test (Linux), not part of the default build: cmake --build <dir> --target tmnt_renderdoc_detect_tests
if(UNIX AND NOT APPLE)
    # The fake librenderdoc.so goes into renderdoc/, out of reach of the runtime's $ORIGIN RUNPATH.
    add_library(tmnt_fake_renderdoc SHARED EXCLUDE_FROM_ALL tests/fake_renderdoc.cpp)
    set_target_properties(tmnt_fake_renderdoc PROPERTIES OUTPUT_NAME renderdoc
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/renderdoc")
    add_executable(tmnt_renderdoc_detect_tests EXCLUDE_FROM_ALL tests/renderdoc_detect_tests.cpp)
    target_link_libraries(tmnt_renderdoc_detect_tests PRIVATE rex::runtime rex::renderdoc ${CMAKE_DL_LIBS})
    target_compile_options(tmnt_renderdoc_detect_tests PRIVATE -Wall -Wextra)
    add_dependencies(tmnt_renderdoc_detect_tests tmnt_fake_renderdoc)
    add_custom_command(TARGET tmnt_renderdoc_detect_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:rex::runtime>"
            "$<TARGET_FILE_DIR:tmnt_renderdoc_detect_tests>"
        VERBATIM
    )
    set_target_properties(tmnt_renderdoc_detect_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
