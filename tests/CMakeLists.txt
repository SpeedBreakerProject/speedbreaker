# The unit tests as CMake targets (-DSB_TESTS=ON at the top level), built as
# each file's header says to build it by hand: -O2 with assertions on, no
# -march. Run them with ctest from the build folder. None needs game files;
# write_watch_test needs ppc/ (ppc_config.h, generated from your own XEX).
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RT ${ROOT}/runtime)
find_package(Threads REQUIRED)

function(sb_test name)
    cmake_parse_arguments(T "" "" "SOURCES;ARGS" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp ${T_SOURCES})
    target_include_directories(${name} PRIVATE ${RT})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    target_compile_options(${name} PRIVATE -O2 -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
endfunction()

sb_test(block_ranges_test)
# Where guest memory and the upload ring go (gpu/memory_plan.h, header only).
sb_test(memory_plan_test)
sb_test(zpd_report_test)
# The resolve shader's 24-bit depth pack (gpu/depth24_glsl.h), header only;
# renderer.cpp is read to check its resolve shader uses it.
sb_test(depth24_pack_test ARGS ${RT}/gpu/renderer.cpp)
# Depth-only draws run no pixel shader (gpu/depth_only.h, header only): the
# sun's shadow pass as the game draws it, and renderer.cpp's Draw using the
# rule (the cars' and tunnel roofs' shadows, v0.1.3).
sb_test(depth_only_test ARGS ${RT}/gpu/renderer.cpp)
# The internal resolution as a ratio, 1.5x included: guest <-> host pixels
# and the scaled resolve memory's layout (gpu/render_scale.h, header only);
# renderer.cpp is read to check its scaled-memory shaders use it.
sb_test(render_scale_test ARGS ${RT}/gpu/renderer.cpp)
# The police vid-cam's side bars and the fill past the 16:9 band
# (gpu/edge_bars.h, header only).
sb_test(edge_bars_test)
# A saved pipeline cache from another GPU, driver or OS version, and the log
# line that says so (gpu/pipeline_cache_header.h, header only).
sb_test(pipeline_cache_header_test)
sb_test(picture_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# Which Vulkan device the presenter draws with, and its log line
# (video/gpu_choice.h, header only).
sb_test(gpu_choice_test)
# The sun's glow fitted to the view scales (game/glare.h, header only), on
# video::FitPicture's pictures.
sb_test(glare_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# The post-processing's split at the 16:9 band's edge (video/band_filter.h,
# video::BandOf) against the edge-bar fills and a model of the filters.
sb_test(band_filter_test SOURCES ${RT}/video/picture_fit.cpp)
# Internal Resolution Auto's budget on iPhones and iPads, chip by chip
# (video/scale_budget.cpp), and the scale it makes on the phones' screens.
sb_test(scale_budget_test SOURCES ${RT}/video/scale_budget.cpp ${RT}/video/picture_fit.cpp)
# Settings > Display > Brightness's curve and the 360's display gamma ramp
# (video/display_tone.h, video/display_gamma.h: the DC_LUT_* registers as
# Direct3D writes them), header only; presenter.cpp is read to check the
# tone shader's wrapping of the scale shaders, settings.cpp for the two
# options' defaults (Xbox 360 Display Gamma on since v0.1.4).
sb_test(display_tone_test ARGS ${RT}/video/presenter.cpp ${RT}/user/settings.cpp)
sb_test(refresh_fit_test SOURCES ${RT}/video/refresh_fit.cpp)
# How late the guest vblank's thread wakes on this machine, alone and with
# every core busy (video/vblank_timer.cpp; checked on macOS, where its
# scheduling changed in v0.1.4).
sb_test(vblank_timer_test SOURCES ${RT}/video/vblank_timer.cpp)
# When the Mac shows a new frame at a set time after its vblank, and the
# watchdog that turns it off (video/present_schedule.h, header only).
sb_test(present_schedule_test)
sb_test(frame_rate_policy_test SOURCES ${RT}/video/frame_rate_policy.cpp)
# When the presenter builds its swapchain again and at what size (an iPhone's
# view turned portrait by SDL, sizes seen while inactive, SUBOPTIMAL), and the
# UI's display size: scenarios, then planted wrong rules that must fail one.
sb_test(swapchain_policy_test SOURCES ${RT}/video/swapchain_policy.cpp)
# The front end's matrix after the window changes shape: the game's
# pointer-keyed matrix cache and the hook's resend (game/front_end_matrix.h,
# header only; the "EVENT HANDLER" label above the boot PSA).
sb_test(front_end_matrix_test)
# Settings > Display > Fullscreen against the window's own state
# (video/fullscreen_policy.h, header only): the green button and other system
# changes never change the saved setting, the menu does; presenter.cpp is read
# to check its window events don't write the setting.
sb_test(fullscreen_policy_test ARGS ${RT}/video/presenter.cpp)
# Game time in whole vblanks (game/whole_vblanks.h), header only.
sb_test(whole_vblanks_test)
# The nitrous light trails' ring at the 360's 30 fps length (game/light_trails.h,
# header only; GitHub issue #10).
sb_test(light_trails_test)
sb_test(guest_time_test SOURCES ${RT}/cpu/guest_time.cpp)
# When the mouse cursor shows over the game (ui/cursor_idle.h, header only),
# and how ui.cpp drives it.
sb_test(cursor_idle_test ARGS ${RT}/ui/ui.cpp)
sb_test(report_zip_test SOURCES ${RT}/report/zip.cpp ARGS ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    # What report_zip_test wrote, read back with independent code.
    add_test(NAME report_zip_check COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/report_zip_check.py
        ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
    set_tests_properties(report_zip_check PROPERTIES DEPENDS report_zip_test)
endif()
# "Choose a folder..." in the installer (install/placement.h): where the game
# goes in a picked folder and what is refused, on planted facts and in a
# scratch folder with a sandbox HOME. POSIX only (mkdtemp, chmod, ACLs and
# links in its disk checks); the screen hides the entry on Windows until
# placement.cpp is ported there.
if(NOT WIN32)
    sb_test(install_folder_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp
        ARGS ${CMAKE_BINARY_DIR}/install_folder_scratch)  # not beside the test: that's its "own folder", refused
    # Portable mode (user/portable.h) and --portable on|off
    # (install/portable_switch.h): the layout, the records that move with
    # the folder, the placement rules inside it, the startup write check, and
    # the copies (a stand-in copies the game), in a scratch folder.
    sb_test(portable_test SOURCES ${RT}/user/portable.cpp ${RT}/install/portable_switch.cpp ${RT}/install/placement.cpp
        ${RT}/install/installer.cpp ${RT}/install/locate.cpp ${RT}/install/manifest.cpp ${RT}/install/result.cpp
        ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ARGS ${CMAKE_BINARY_DIR}/portable_scratch)
    if(APPLE AND NOT IOS)
        target_link_libraries(install_folder_test PRIVATE "-framework CoreFoundation")  # placement.cpp's iCloud check
        target_link_libraries(portable_test PRIVATE "-framework CoreFoundation")
    endif()
endif()
# The SteamOS launcher's hybrid-graphics part (NVIDIA PRIME render offload,
# scripts/steamos/bundle/speedbreaker.sh) on made-up /sys, /proc and Vulkan
# folders. Bash, so not on Windows.
if(NOT WIN32)
    find_program(SB_BASH bash)
    if(SB_BASH)
        add_test(NAME launcher_prime_test COMMAND ${SB_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_prime_test.sh
            ${ROOT}/scripts/steamos/bundle/speedbreaker.sh)
    endif()
endif()
# The entitlements in a Mach-O's code signature (platform/code_signature.h),
# on files built byte by byte: what the iOS app reads from its own.
sb_test(code_signature_test SOURCES ${RT}/platform/code_signature.cpp)
# The [memory] line, the page shown when the guest's 4 GB can't be reserved
# (an iOS copy without Extended Virtual Addressing) and the low-memory notice
# (kernel/memory_status.h).
sb_test(memory_status_test SOURCES ${RT}/kernel/memory_status.cpp ${RT}/platform/code_signature.cpp)
# The version line in the log, and when the "What's new" note shows
# (user/version_check.h, header only).
sb_test(version_check_test)
# The keys a Controls row can hold and their names in settings.toml
# (user/key_names.h, header only), each row against SDL's scancode name.
find_package(SDL3 REQUIRED CONFIG)
sb_test(key_names_test)
target_link_libraries(key_names_test PRIVATE SDL3::SDL3)
# The Key options (Settings > Controls): the table, the [keys] names in the
# file, and what Set accepts (user/settings.cpp and the toml++ XenonRecomp
# vendors; no SDL).
sb_test(settings_keys_test SOURCES ${RT}/user/settings.cpp)
target_include_directories(settings_keys_test SYSTEM PRIVATE ${ROOT}/tools/XenonRecomp/thirdparty/tomlplusplus/include)
# The keyboard's controls and how held keys become a pad (hid/key_map.h), header only.
sb_test(key_map_test)
# Check for Updates' logic: versions, GitHub's JSON, curl's output and the
# release notes (update/check.cpp, self-contained).
sb_test(update_check_test SOURCES ${RT}/update/check.cpp)
# Settings > Quit Game: where it is offered (not on iPhone or iPad) and its
# confirmation (ui/quit_game.h, header only); the runtime's code around it
# is read: the button and prompt gated, the focus on Cancel, B and Escape
# cancelling, and the quit taking the window close's way out.
sb_test(quit_game_test ARGS ${RT})
# The jump tables XenonRecomp makes switches of (config/switch_tables.toml)
# against the leaf functions' bounds (config/nfsmw.toml): no label or default
# outside its function, and the tables a borrowed bounds check broke (the
# Performance Tuning labels, GitHub issue #8) as the guest dispatches them.
sb_test(switch_tables_test ARGS ${ROOT}/config/switch_tables.toml ${ROOT}/config/nfsmw.toml)
# The write watch around CPU write faults, on physical memory mapped as the
# runtime maps it.
sb_test(write_watch_test SOURCES ${RT}/kernel/write_watch.cpp ${RT}/cpu/guest_time.cpp)
target_include_directories(write_watch_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
# The guest vblank lock's closed-loop simulation (a tool more than a test).
add_executable(vblank_lock_sim ${CMAKE_CURRENT_SOURCE_DIR}/vblank_lock_sim.cpp ${RT}/video/refresh_fit.cpp ${RT}/video/vblank_lock.cpp)
target_include_directories(vblank_lock_sim PRIVATE ${RT})
