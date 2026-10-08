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
sb_test(zpd_report_test)
sb_test(picture_fit_test SOURCES ${RT}/video/picture_fit.cpp)
sb_test(refresh_fit_test SOURCES ${RT}/video/refresh_fit.cpp)
sb_test(frame_rate_policy_test SOURCES ${RT}/video/frame_rate_policy.cpp)
# Game time in whole vblanks (game/whole_vblanks.h), header only.
sb_test(whole_vblanks_test)
sb_test(guest_time_test SOURCES ${RT}/cpu/guest_time.cpp)
sb_test(report_zip_test SOURCES ${RT}/report/zip.cpp ARGS ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    # What report_zip_test wrote, read back with independent code.
    add_test(NAME report_zip_check COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/report_zip_check.py
        ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
    set_tests_properties(report_zip_check PROPERTIES DEPENDS report_zip_test)
endif()
# The version line in the log, and when the "What's new" note shows
# (user/version_check.h, header only).
sb_test(version_check_test)
# The write watch around CPU write faults, on physical memory mapped as the
# runtime maps it.
sb_test(write_watch_test SOURCES ${RT}/kernel/write_watch.cpp ${RT}/cpu/guest_time.cpp)
target_include_directories(write_watch_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
# The guest vblank lock's closed-loop simulation (a tool more than a test).
add_executable(vblank_lock_sim ${CMAKE_CURRENT_SOURCE_DIR}/vblank_lock_sim.cpp ${RT}/video/refresh_fit.cpp ${RT}/video/vblank_lock.cpp)
target_include_directories(vblank_lock_sim PRIVATE ${RT})
