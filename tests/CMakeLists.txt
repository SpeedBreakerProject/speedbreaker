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
    if(SB_MSVC_FRONTEND)
        target_compile_options(${name} PRIVATE /O2 -UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
endfunction()

# user/settings.cpp makes its file calls through the platform layer
# (platform/file_io.h, the Windows port's): each test that links it links
# that layer as well (Windows: its folders too, with their system libraries).
if(WIN32)
    set(SB_SETTINGS_SOURCES ${RT}/user/settings.cpp ${RT}/platform/win32/file_io.cpp ${RT}/platform/win32/folders.cpp)
    set(SB_SETTINGS_LIBS shell32 ole32)
else()
    set(SB_SETTINGS_SOURCES ${RT}/user/settings.cpp ${RT}/platform/posix/file_io.cpp)
    set(SB_SETTINGS_LIBS)
endif()

sb_test(block_ranges_test)
# Where guest memory and the upload ring go (gpu/memory_plan.h, header only).
sb_test(memory_plan_test)
# A discrete GPU's PCIe link for the logs (gpu/pcie_link.h, header only; issue #40).
sb_test(pcie_link_test)
# Deferred shadow resolves (gpu/shadow_lend.h, header only): their decisions,
# and a model of the renderer's against resolving every time.
sb_test(shadow_lend_test)
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
# Split screen's two views in one untiled target (gpu/split_views.h, header
# only): each cut to its own rows at 1x, 1.5x, 2x, 3x and Vert+'s taller y
# scales, meeting exactly; renderer.cpp is read to check it applies the rules.
sb_test(split_views_test ARGS ${RT}/gpu/renderer.cpp)
# Which controller is which player in split screen (hid/player_slots.h,
# header only) and the game's split-screen test (game/split_screen.h);
# input.cpp is read to check users 1-3 follow it.
sb_test(player_slots_test ARGS ${RT}/hid/input.cpp)
# The police vid-cam's side bars, the intro's black frames, the results
# screen's backdrop and what fills the screen round them (gpu/edge_bars.h,
# header only).
sb_test(edge_bars_test)
# GitHub #19's menu borders (gpu/edge_bars.h, header only): the Showcase's
# vignette stretched to the screen's edges, the Blacklist bio's black panel
# filled all round, at every screen shape; renderer.cpp is read to check it
# asks for and applies the plans on both paths.
sb_test(menu_borders_test ARGS ${RT}/gpu/renderer.cpp)
# The fade to black before a load short of the screen's edges (the career
# prologue's last race, v0.1.7 on a MacBook): stretched to them at every
# screen shape (gpu/edge_bars.h's PlanLayer); renderer.cpp is read to check
# it asks for and applies it on both paths.
sb_test(fade_layers_test ARGS ${RT}/gpu/renderer.cpp)
# NFSMW_LOG_COVERS (gpu/covers.h, header only): front-end quads and presented
# pictures that leave a dark box, or a full-screen layer, short of the
# picture's edges.
sb_test(covers_test)
# A saved pipeline cache from another GPU, driver or OS version, and the log
# line that says so (gpu/pipeline_cache_header.h, header only).
sb_test(pipeline_cache_header_test)
sb_test(picture_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# GitHub #34's native rows and columns (video::NativeShape, AutoNativeScale):
# a Steam Deck's 1280x800 at 1x renders 1280x800, 16:9 pictures keep their
# scale exactly, Auto takes the native shape within its budget.
sb_test(native_rows_test SOURCES ${RT}/video/picture_fit.cpp)
# Settings > Video > Internal Resolution's labels name the size each entry
# renders on this screen (video::ResolutionLabel): 16:9, a Steam Deck filled
# and at 16:9, 21:9; the settings file's tokens unchanged, the menu using them.
sb_test(resolution_label_test SOURCES ${RT}/video/picture_fit.cpp
    ARGS ${RT}/user/settings.cpp ${RT}/ui/settings_menu.cpp ${RT}/gpu/renderer.cpp)
# Which Vulkan device the presenter draws with, and its log line
# (video/gpu_choice.h, header only).
sb_test(gpu_choice_test)
# The sun's glow fitted to the view scales (game/glare.h, header only), on
# video::FitPicture's pictures.
sb_test(glare_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# The post-processing's split at the 16:9 band's edge (video/band_filter.h,
# video::BandOf) against the edge-bar fills and a model of the filters
# (FSR 1's EASU taps and RCAS neighbours too).
sb_test(band_filter_test SOURCES ${RT}/video/picture_fit.cpp)
# Settings > Graphics > Upscaling's FSR 1: which pass draws a frame
# (video/upscale.h, header only, on video::FitPicture's pictures), Sharpening
# as RCAS's strength, the constants against AMD's formulas, and the shaders'
# GLSL (video/fsr1.cpp, with runtime/generated/fsr1_headers.inc): AMD's
# headers compiled in unchanged, the band rules in their code, the
# presenter's use of them. It writes the GLSL to fsr1_glsl for the check
# after it.
sb_test(fsr1_test SOURCES ${RT}/video/fsr1.cpp ${RT}/video/picture_fit.cpp
    ARGS ${RT}/thirdparty/fsr1 ${RT}/video ${CMAKE_CURRENT_BINARY_DIR}/fsr1_glsl)
target_include_directories(fsr1_test PRIVATE ${CMAKE_BINARY_DIR}/runtime/generated)
# Those shaders compiled to SPIR-V for Vulkan, as the game's glslang does
# (only where glslangValidator is installed).
find_program(SB_GLSLANG_VALIDATOR glslangValidator)
if(SB_GLSLANG_VALIDATOR)
    foreach(shader easu.comp rcas.frag rcas_tone.frag)
        add_test(NAME fsr1_glsl_check_${shader} COMMAND ${SB_GLSLANG_VALIDATOR} -V --target-env vulkan1.1
            ${CMAKE_CURRENT_BINARY_DIR}/fsr1_glsl/${shader} -o ${CMAKE_CURRENT_BINARY_DIR}/fsr1_glsl/${shader}.spv)
        set_tests_properties(fsr1_glsl_check_${shader} PROPERTIES DEPENDS fsr1_test)
    endforeach()
endif()
# Upscaling's values in settings.toml (user/settings.cpp as the game links
# it): "fsr1" read, saved and read back, the environment's override, an
# unknown value; in a scratch folder.
sb_test(settings_scaling_test SOURCES ${SB_SETTINGS_SOURCES} ARGS ${CMAKE_CURRENT_BINARY_DIR}/settings_scaling_scratch)
target_link_libraries(settings_scaling_test PRIVATE ${SB_SETTINGS_LIBS})
# Internal Resolution Auto's budget on iPhones and iPads, chip by chip
# (video/scale_budget.cpp), and the scale it makes on the phones' screens.
sb_test(scale_budget_test SOURCES ${RT}/video/scale_budget.cpp ${RT}/video/picture_fit.cpp)
# Settings > Display > Brightness's curve and the 360's display gamma ramp
# (video/display_tone.h, video/display_gamma.h: the DC_LUT_* registers as
# Direct3D writes them), header only; presenter.cpp is read to check the
# tone shader's wrapping of the scale shaders, settings.cpp for the two
# options' defaults (Xbox 360 Display Gamma on since v0.1.4).
sb_test(display_tone_test ARGS ${RT}/video/presenter.cpp ${RT}/user/settings.cpp)
# The display's refresh measured from present completions, at 60 Hz and at
# whole multiples of it (video/refresh_fit.cpp, video/refresh_multiple.h).
sb_test(refresh_fit_test SOURCES ${RT}/video/refresh_fit.cpp)
# The [perf] "repeated" count in the panel's own refreshes (video/repeats.h,
# header only; the Windows presenter's).
sb_test(repeats_test)
# The one-time notice on a screen whose rate 60 fps can't divide evenly (90,
# 144, 165 Hz): when it shows (variable refresh, Game Mode, a Mac's ProMotion,
# dismissed, the moment), its words, the grid check under it, and the
# dismissal in settings.toml, in a scratch folder (video/refresh_notice.h,
# user/settings.cpp).
sb_test(refresh_notice_test SOURCES ${RT}/video/refresh_fit.cpp ${SB_SETTINGS_SOURCES}
    ARGS ${CMAKE_CURRENT_BINARY_DIR}/refresh_notice_scratch)
target_link_libraries(refresh_notice_test PRIVATE ${SB_SETTINGS_LIBS})
# The one-time note that an Xbox controller by USB on a Mac takes no rumble
# (hid/rumble_note.h): a wired Xbox pad on a Mac gets it once, a DualSense
# wired, an Xbox pad over Bluetooth and other platforms never; the dismissal
# in settings.toml, in a scratch folder (user/settings.cpp).
sb_test(rumble_note_test SOURCES ${SB_SETTINGS_SOURCES}
    ARGS ${CMAKE_CURRENT_BINARY_DIR}/rumble_note_scratch)
target_link_libraries(rumble_note_test PRIVATE ${SB_SETTINGS_LIBS})
# How late the guest vblank's thread wakes on this machine, alone and with
# every core busy (video/vblank_timer.cpp; checked on macOS, where its
# scheduling changed in v0.1.4).
sb_test(vblank_timer_test SOURCES ${RT}/video/vblank_timer.cpp)
# When the Mac shows a new frame at a set time after its vblank, and the
# watchdog that turns it off (video/present_schedule.h, header only).
sb_test(present_schedule_test)
# The audio render callback's cadence against the output device
# (apu/audio_cadence.h, header only): a device that stops taking audio
# doesn't stop the game's sound (GitHub issue #20: the sound stopped for good,
# then the game hung after the race), and a working one is followed as before.
sb_test(audio_cadence_test)
# EA SND's mixer never drops sounds for time on a host (apu/ea_cpu_budget.h,
# header only; GitHub issue #35: one sound, usually the engine, much quieter
# until the next gear change): EA's budget rule as a model, the policy, and
# the generated code (ppc/) read to check the functions and offsets it names.
sb_test(ea_cpu_budget_test ARGS ${ROOT}/ppc)
# The present waiter uses a swapchain only while it is the current one, and
# none is destroyed under such a use (video/swapchain_use.h, header only):
# fake swapchains destroyed and made again under a waiting thread; presenter.cpp
# is read to check every waiter call that takes a swapchain is inside a use.
sb_test(swapchain_use_test ARGS ${RT}/video/presenter.cpp)
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
# SPIKE (v017-hfr-spike): the in-between picture's pose blending (game/hfr_blend.h, header only).
sb_test(hfr_blend_test)
sb_test(hfr_schedule_test)
sb_test(hfr_grid_test)
# How long Smooth Motion stays at 60 after a picture on the wrong refresh, off
# Apple's systems (game/hfr_late_hold.h, header only).
sb_test(hfr_late_hold_test)
sb_test(guest_time_test SOURCES ${RT}/cpu/guest_time.cpp)
if(WIN32)
    target_link_libraries(guest_time_test PRIVATE winmm)  # timeBeginPeriod
endif()
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
# Crash and hang reports: which symbols are recompiled functions, and the
# home folder scrubbed from a bug report (both header only).
sb_test(symbol_names_test)
sb_test(scrub_test)
if(WIN32)
    # Windows' stacks and names (platform/stack.h: DbgHelp, StackWalk64,
    # another thread's stack), against this test's own PDB: its main file
    # with debug information, and a file of functions shaped as the
    # recompiled ones without any, as ppc/ is built.
    sb_test(win_stack_test SOURCES ${RT}/platform/win32/stack.cpp ${CMAKE_CURRENT_SOURCE_DIR}/win_stack_test_guest.cpp)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/win_stack_test.cpp ${RT}/platform/win32/stack.cpp PROPERTIES
        COMPILE_OPTIONS /Z7)
    target_link_options(win_stack_test PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
    target_link_libraries(win_stack_test PRIVATE dbghelp)
endif()
# Where the per-user folders go, and the platform layer's file calls.
sb_test(paths_test)
if(WIN32)
    sb_test(file_io_test SOURCES ${RT}/platform/win32/file_io.cpp ARGS ${CMAKE_CURRENT_BINARY_DIR}/file_io_out)
    target_sources(paths_test PRIVATE ${RT}/platform/win32/folders.cpp)
    target_link_libraries(paths_test PRIVATE shell32 ole32)
else()
    sb_test(file_io_test SOURCES ${RT}/platform/posix/file_io.cpp ARGS ${CMAKE_CURRENT_BINARY_DIR}/file_io_out)
endif()
# Settings: a device's own defaults, before and after Load.
if(WIN32)
    sb_test(settings_test SOURCES ${RT}/user/settings.cpp ${RT}/platform/win32/file_io.cpp ${RT}/platform/win32/folders.cpp
        ARGS ${CMAKE_CURRENT_BINARY_DIR}/settings_out)
    target_link_libraries(settings_test PRIVATE shell32 ole32)
else()
    sb_test(settings_test SOURCES ${RT}/user/settings.cpp ${RT}/platform/posix/file_io.cpp
        ARGS ${CMAKE_CURRENT_BINARY_DIR}/settings_out)
endif()
target_include_directories(settings_test SYSTEM PRIVATE ${ROOT}/tools/XenonRecomp/thirdparty/tomlplusplus/include)
# "Choose a folder..." in the installer (install/placement.h): where the game
# goes in a picked folder and what is refused, on planted facts and in a
# scratch folder with a sandbox HOME (the installer's file calls through the
# platform layer, platform/file_io.h). POSIX only (mkdtemp, chmod, ACLs and
# links in its disk checks); on Windows install_folder_win_test checks the
# Windows side on a real disk (a drive's root, OneDrive, writability).
if(WIN32)
    sb_test(install_folder_win_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp ${RT}/platform/win32/file_io.cpp ${RT}/platform/win32/folders.cpp
        ARGS ${CMAKE_BINARY_DIR}/install_folder_scratch)  # not beside the test: that's its "own folder", refused
    target_link_libraries(install_folder_win_test PRIVATE shell32 ole32)
endif()
if(NOT WIN32)
    sb_test(install_folder_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp ${RT}/platform/posix/file_io.cpp
        ARGS ${CMAKE_BINARY_DIR}/install_folder_scratch)  # not beside the test: that's its "own folder", refused
    # Portable mode (user/portable.h) and --portable on|off
    # (install/portable_switch.h): the layout, the records that move with
    # the folder, the placement rules inside it, the startup write check, and
    # the copies (a stand-in copies the game), in a scratch folder.
    sb_test(portable_test SOURCES ${RT}/user/portable.cpp ${RT}/install/portable_switch.cpp ${RT}/install/placement.cpp
        ${RT}/install/installer.cpp ${RT}/install/locate.cpp ${RT}/install/manifest.cpp ${RT}/install/result.cpp
        ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp ${RT}/platform/posix/file_io.cpp
        ARGS ${CMAKE_BINARY_DIR}/portable_scratch)
    # The installer's regions (install/manifest.h): the USA and the PAL
    # (Spanish) disc told apart, and either's install found, in a scratch
    # folder with a sandbox HOME.
    sb_test(region_manifest_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp ${RT}/platform/posix/file_io.cpp
        ARGS ${CMAKE_BINARY_DIR}/region_manifest_scratch)
    if(APPLE AND NOT IOS)
        target_link_libraries(install_folder_test PRIVATE "-framework CoreFoundation")  # placement.cpp's iCloud check
        target_link_libraries(portable_test PRIVATE "-framework CoreFoundation")
        target_link_libraries(region_manifest_test PRIVATE "-framework CoreFoundation")
    endif()
endif()
# The PAL (Spanish) release's hook addresses (runtime/region/region.h):
# every guest address the runtime names has a PAL one, every hook is an
# SB_HOOK, code and .rdata constants go through region::Addr, none is in a
# function the PAL build changed. -DSB_REGION_SURVEY=<folder with mw_usa/ and
# mw_pal/ (image.bin, xexsurvey.txt; mw_pal/funcmap.tsv)>, decrypted from the
# player's own discs, also checks the bytes at both addresses match and that
# the table and config/pal-es/ are what survey/region_map.py writes.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(SB_REGION_SURVEY "" CACHE PATH "Decrypted USA and PAL images for region_addresses_check (optional)")
    if(SB_REGION_SURVEY)
        add_test(NAME region_addresses_check COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/region_addresses_check.py
            ${ROOT} ${SB_REGION_SURVEY}/mw_usa ${SB_REGION_SURVEY}/mw_pal ${SB_REGION_SURVEY}/mw_pal/funcmap.tsv)
    else()
        add_test(NAME region_addresses_check COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/region_addresses_check.py ${ROOT})
    endif()
    # scripts/region_ppc.py on both forms of XenonRecomp's output (a Mac's
    # wrapper form; Linux's and clang-cl's alias form, with LF or CRLF): the
    # same namespaced files from each.
    add_test(NAME region_ppc_test COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/region_ppc_test.py ${ROOT}/scripts/region_ppc.py)
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
# The only test that links SDL3, so it keeps the build tree's path to it even
# with -DCMAKE_SKIP_BUILD_RPATH=ON: the Steam Frame's cmake line
# (scripts/steamos/README.md) keeps ~/deps/sdl3/lib out of the game that
# way, and ctest runs the tests without LD_LIBRARY_PATH, so this one couldn't
# load libSDL3 there (v0.1.5). Only this test: the game's own rpath is
# unchanged (none on the Frame).
find_package(SDL3 REQUIRED CONFIG)
sb_test(key_names_test)
target_link_libraries(key_names_test PRIVATE SDL3::SDL3)
set_target_properties(key_names_test PROPERTIES SKIP_BUILD_RPATH OFF)
# The Key options (Settings > Controls): the table, the [keys] names in the
# file, and what Set accepts (user/settings.cpp and the toml++ XenonRecomp
# vendors; no SDL).
sb_test(settings_keys_test SOURCES ${SB_SETTINGS_SOURCES})
target_link_libraries(settings_keys_test PRIVATE ${SB_SETTINGS_LIBS})
target_include_directories(settings_keys_test SYSTEM PRIVATE ${ROOT}/tools/XenonRecomp/thirdparty/tomlplusplus/include)
# Settings > Game > Language (game/language.h, header only, and the option in
# user/settings.cpp): English untouched for every caller, the game's ten
# languages, the movie suffix by the files present, what the files give.
sb_test(language_test SOURCES ${SB_SETTINGS_SOURCES})
target_link_libraries(language_test PRIVATE ${SB_SETTINGS_LIBS})
target_include_directories(language_test SYSTEM PRIVATE ${ROOT}/tools/XenonRecomp/thirdparty/tomlplusplus/include)
# The keyboard's controls and how held keys become a pad (hid/key_map.h), header only.
sb_test(key_map_test)
# A USB steering wheel as the pad (hid/wheel_map.h, header only): which
# joystick is a wheel, which of its axes is the wheel and which are its
# pedals, the steering curve, the pedals' travel, the buttons and paddles,
# NFSMW_WHEEL_BUTTONS, and the force-feedback levels.
sb_test(wheel_map_test)
# Check for Updates' logic: versions, GitHub's JSON, curl's output and the
# release notes (update/check.cpp, self-contained).
sb_test(update_check_test SOURCES ${RT}/update/check.cpp)
# Settings > Quit Game: where it is offered (not on iPhone or iPad) and its
# confirmation (ui/quit_game.h, header only); the runtime's code around it
# is read: the button and prompt gated, the focus on Cancel, B and Escape
# cancelling, and the quit taking the window close's way out.
sb_test(quit_game_test ARGS ${RT})
# A requested termination (SIGTERM: a launcher's stop, a shutdown, kill;
# SIGINT; SIGHUP) quits as the window's close button does, and only a session that
# didn't end cleanly leaves its crash marker (platform/quit_signals.h,
# report/crash_marker.h, header only): signals in child processes, the marker
# in a scratch folder, SIGTERM vs SIGKILL end to end; main.cpp, presenter.cpp
# and crash.cpp are read to check the quit paths use them. POSIX only (fork).
if(NOT WIN32)
    sb_test(clean_exit_test ARGS ${CMAKE_CURRENT_BINARY_DIR}/clean_exit_scratch ${RT})
endif()
# The jump tables XenonRecomp makes switches of (config/switch_tables.toml)
# against the leaf functions' bounds (config/nfsmw.toml): no label or default
# outside its function, and the tables a borrowed bounds check broke (the
# Performance Tuning labels, GitHub issue #8) as the guest dispatches them.
sb_test(switch_tables_test ARGS ${ROOT}/config/switch_tables.toml ${ROOT}/config/nfsmw.toml)
# The write watch around CPU write faults, on physical memory mapped as the
# runtime maps it (three windows of one shared object).
sb_test(write_watch_test SOURCES ${RT}/kernel/write_watch.cpp ${RT}/cpu/guest_time.cpp)
target_include_directories(write_watch_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
# Released and decommitted guest pages back to zero on hosts with larger pages
# (16 KB on Apple): only whole host pages remapped, so a 4 KB-page heap's
# neighbouring allocation in the same host page survives (kernel/vmem.cpp; the
# host page size injected, so 16 and 64 KB are checked on any host).
if(NOT WIN32)  # POSIX mappings; Windows' host pages are 4 KB (guest pages)
sb_test(vmem_host_pages_test SOURCES ${RT}/kernel/vmem.cpp)
target_include_directories(vmem_host_pages_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
endif()
if(WIN32)
    # The mirrors (platform/win32/memory.cpp) and the page protection.
    target_sources(write_watch_test PRIVATE ${RT}/platform/win32/memory.cpp ${RT}/platform/win32/os.cpp)
    # The process-wide settings main makes first (platform/startup.h).
    sb_test(startup_test SOURCES ${RT}/platform/win32/startup.cpp)
endif()
# The guest vblank lock's closed-loop simulation (a tool more than a test).
add_executable(vblank_lock_sim ${CMAKE_CURRENT_SOURCE_DIR}/vblank_lock_sim.cpp ${RT}/video/refresh_fit.cpp ${RT}/video/vblank_lock.cpp)
target_include_directories(vblank_lock_sim PRIVATE ${RT})
target_compile_options(vblank_lock_sim PRIVATE -O2 -UNDEBUG)
# Its displays at a whole multiple of 60 Hz (120, 180, 240: every 2nd-4th
# refresh, frames held for theirs), the rates that aren't, variable refresh,
# Frame Rate 30, Windows' late vblank wake-ups, and planted faults (about 40 s;
# the 60 Hz tables, a minute and a half, by hand).
add_test(NAME vblank_lock_sim_multiples COMMAND vblank_lock_sim --multiples)
