# CMake build for GNU Pth (alternative to the autoconf build).
# Replicates configure.ac: feature checks, generated headers
# (pth.h, pth_acdef.h, pth_acmac.h, pth_p.h) and the library/test builds.

cmake_minimum_required(VERSION 3.19)

# -- version, extracted from pth_vers.c -------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pth_vers.c" _vers_content)
string(REGEX MATCH "0x[0-9A-Fa-f]+" PTH_VERSION_HEX "${_vers_content}")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+[.ab][0-9]+) \\(([^)]*)\\)\"" _vers_match "${_vers_content}")
set(PTH_VERSION_STR "${CMAKE_MATCH_1} (${CMAKE_MATCH_2})")
set(PTH_VERSION_SHORT "${CMAKE_MATCH_1}")

project(pth VERSION ${PTH_VERSION_SHORT} LANGUAGES C)

if(WIN32 AND NOT CYGWIN)
    message(FATAL_ERROR "GNU Pth requires a POSIX platform (Linux, macOS, BSD, WSL, Cygwin).")
endif()

# -- options -----------------------------------------------------------------
option(PTH_BUILD_TESTS   "Build test programs"                        ON)
option(PTH_BUILD_PTHREAD "Build POSIX pthread emulation library"      OFF)
option(PTH_DEBUG         "Enable Pth debugging"                       OFF)
option(PTH_SYSCALL_SOFT  "Enable soft system call mapping"            OFF)
option(PTH_SYSCALL_HARD  "Enable hard system call mapping"            OFF)
option(PTH_MP            "Enable multi-scheduler (multi-CPU) support" ON)
option(PTH_SCHED_POLL   "Use poll(2) not select(2) in the scheduler core (no FD_SETSIZE ceiling)" ON)
# BUILD_SHARED_LIBS selects static (default) vs shared library

# mctx overrides (equivalent to --with-mctx-mth/dsp/stk); empty = autodetect
set(PTH_MCTX_MTH "" CACHE STRING "Force mctx method (mcsc, sjlj)")
set(PTH_MCTX_DSP "" CACHE STRING "Force mctx dispatching (sc, ssjlj, sjlj, usjlj, sjlje, sjljlx, sjljisc, sjljw32)")
set(PTH_MCTX_STK "" CACHE STRING "Force mctx stack setup (mc, ss, sas, none)")

if(PTH_BUILD_PTHREAD)
    set(PTH_SYSCALL_SOFT ON)  # configure.ac forces this for --enable-pthread
endif()
if(PTH_MP AND PTH_BUILD_PTHREAD)
    # The emulation owns the pthread_* symbols, so the MP core reaches the
    # real OS-thread primitives via dlsym(RTLD_NEXT) (see pth_sched.c, guarded
    # by PTH_EMULATION). pthread_create(3) then round-robins new threads
    # across the schedulers.
    message(STATUS "Building pthread emulation with multi-scheduler (PTH_MP) support")
endif()

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckStructHasMember)
include(CheckLibraryExists)
include(CheckCSourceCompiles)

# -- header checks -----------------------------------------------------------
foreach(hdr
    dlfcn.h errno.h fcntl.h inttypes.h memory.h net/errno.h paths.h
    pthread.h setjmp.h signal.h stdarg.h stdint.h stdio.h stdlib.h
    strings.h string.h sys/resource.h sys/select.h sys/socketcall.h
    sys/socket.h sys/stat.h sys/syscall.h sys/time.h sys/types.h
    sys/uio.h sys/wait.h unistd.h ucontext.h)
    string(TOUPPER "${hdr}" _var)
    string(REGEX REPLACE "[/.]" "_" _var "${_var}")
    check_include_file("${hdr}" "HAVE_${_var}")
endforeach()

foreach(hdr stdio.h stdlib.h stdarg.h string.h signal.h unistd.h setjmp.h
        fcntl.h errno.h sys/types.h sys/time.h sys/wait.h sys/stat.h sys/socket.h)
    string(TOUPPER "${hdr}" _var)
    string(REGEX REPLACE "[/.]" "_" _var "${_var}")
    if(NOT HAVE_${_var})
        message(FATAL_ERROR "mandatory system header <${hdr}> not found")
    endif()
endforeach()
set(STDC_HEADERS 1)

# -- function checks ---------------------------------------------------------
foreach(fn
    gettimeofday select sigaction sigprocmask sigpending sigsuspend
    poll readv writev usleep strerror
    makecontext swapcontext getcontext setcontext
    sigsetjmp siglongjmp setjmp longjmp _setjmp _longjmp
    sigaltstack sigstack syscall dlopen dlsym dlclose)
    string(TOUPPER "${fn}" _var)
    check_function_exists("${fn}" "HAVE_${_var}")
endforeach()

foreach(fn gettimeofday select sigaction sigprocmask sigpending sigsuspend)
    string(TOUPPER "${fn}" _var)
    if(NOT HAVE_${_var})
        message(FATAL_ERROR "mandatory function ${fn}(2) not found")
    endif()
endforeach()

# sigsetjmp is often a macro (e.g. glibc), which check_function_exists misses
if(NOT HAVE_SIGSETJMP)
    check_c_source_compiles("
        #include <setjmp.h>
        int main(void) { sigjmp_buf b; if (sigsetjmp(b,1)) return 1; return 0; }"
        HAVE_SIGSETJMP_AS_MACRO)
    if(HAVE_SIGSETJMP_AS_MACRO)
        set(HAVE_SIGSETJMP 1)
    endif()
endif()
if(HAVE_SIGSETJMP AND NOT HAVE_SIGLONGJMP)
    set(HAVE_SIGLONGJMP 1)   # they come as a pair
endif()

# -- library checks ----------------------------------------------------------
set(PTH_EXTRA_LIBS "")
check_library_exists(dl dlsym "" HAVE_LIBDL)
if(HAVE_LIBDL)
    list(APPEND PTH_EXTRA_LIBS dl)
endif()
check_library_exists(nsl gethostname "" HAVE_LIBNSL)
if(HAVE_LIBNSL)
    list(APPEND PTH_EXTRA_LIBS nsl)
endif()
check_library_exists(socket accept "" HAVE_LIBSOCKET)
if(HAVE_LIBSOCKET)
    list(APPEND PTH_EXTRA_LIBS socket)
endif()

# -- type checks -------------------------------------------------------------
macro(pth_check_type type header var fallback_decl fallback_var)
    set(CMAKE_EXTRA_INCLUDE_FILES "${header}")
    check_type_size("${type}" "${var}")
    unset(CMAKE_EXTRA_INCLUDE_FILES)
    if(HAVE_${var})
        set(${fallback_var} "/* ${fallback_decl} */")
    else()
        set(${fallback_var} "${fallback_decl}")
    endif()
endmacro()

pth_check_type(sig_atomic_t signal.h    SIG_ATOMIC_T "typedef int sig_atomic_t;"          FALLBACK_SIG_ATOMIC_T)
pth_check_type(pid_t        sys/types.h PID_T        "typedef int pid_t;"                  FALLBACK_PID_T)
pth_check_type(size_t       stdlib.h    SIZE_T       "typedef unsigned int size_t;"        FALLBACK_SIZE_T)
pth_check_type(ssize_t      sys/types.h SSIZE_T      "typedef unsigned int ssize_t;"       FALLBACK_SSIZE_T)
pth_check_type(off_t        sys/types.h OFF_T        "typedef int off_t;"                  FALLBACK_OFF_T)
pth_check_type(socklen_t    sys/socket.h SOCKLEN_T   "typedef int socklen_t;"              FALLBACK_SOCKLEN_T)
pth_check_type(nfds_t       poll.h      NFDS_T       "typedef unsigned int nfds_t;"        FALLBACK_NFDS_T)

set(CMAKE_EXTRA_INCLUDE_FILES signal.h)
check_type_size(stack_t STACK_T)
unset(CMAKE_EXTRA_INCLUDE_FILES)

check_struct_has_member("struct sigaltstack" ss_sp   sys/signal.h HAVE_SS_SP)
check_struct_has_member("struct sigaltstack" ss_base sys/signal.h HAVE_SS_BASE)

check_c_source_compiles("
    #include <time.h>
    #include <sys/time.h>
    int main(void) { struct timespec ts; (void)ts; return 0; }"
    HAVE_STRUCT_TIMESPEC)

check_c_source_compiles("
    int main(void) { long long x = 0; (void)x; return 0; }"
    HAVE_LONGLONG)
check_c_source_compiles("
    int main(void) { long double x = 0; (void)x; return 0; }"
    HAVE_LONGDOUBLE)

check_c_source_compiles("
    #include <sys/types.h>
    #include <sys/time.h>
    #include <unistd.h>
    int main(void) { struct timeval tv; (void)gettimeofday(&tv); return 0; }"
    HAVE_GETTIMEOFDAY_ARGS1)

# -- preprocessor define checks ----------------------------------------------
check_symbol_exists(POLLIN    poll.h        HAVE_POLLIN)
check_symbol_exists(RTLD_NEXT dlfcn.h       HAVE_RTLD_NEXT)
check_symbol_exists(SYS_read  sys/syscall.h HAVE_SYS_READ)

# -- fake poll(2) / readv(2)+writev(2) ---------------------------------------
if(HAVE_POLL AND HAVE_POLLIN)
    set(PTH_FAKE_POLL 0)
else()
    set(PTH_FAKE_POLL 1)
endif()
if(HAVE_READV AND HAVE_WRITEV AND HAVE_SYS_UIO_H)
    set(PTH_FAKE_RWV 0)
else()
    set(PTH_FAKE_RWV 1)
endif()

# -- runtime probes ----------------------------------------------------------
set(_probes "${CMAKE_CURRENT_SOURCE_DIR}/cmake/probes")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/probes")

function(pth_run_probe src outvar default)
    if(CMAKE_CROSSCOMPILING)
        set(${outvar} "${default}" PARENT_SCOPE)
        return()
    endif()
    set(_defflags "")
    if(ARGN)
        set(_defflags COMPILE_DEFINITIONS ${ARGN})
    endif()
    try_run(_run _compile "${CMAKE_CURRENT_BINARY_DIR}/probes" "${src}"
            ${_defflags}
            RUN_OUTPUT_VARIABLE _out)
    if(_compile AND _run EQUAL 0)
        string(STRIP "${_out}" _out)
        set(${outvar} "${_out}" PARENT_SCOPE)
    else()
        set(${outvar} "${default}" PARENT_SCOPE)
    endif()
endfunction()

pth_run_probe("${_probes}/nsig.c"        PTH_NSIG        32)
pth_run_probe("${_probes}/fdsetsize.c"   PTH_FDSETSIZE   1024)
pth_run_probe("${_probes}/stackgrowth.c" _stackgrowth    down)
if(_stackgrowth STREQUAL "up")
    set(PTH_STACKGROWTH +1)
    set(PTH_STACK_GROWTH "up")
else()
    set(PTH_STACKGROWTH -1)
    set(PTH_STACK_GROWTH "down")
endif()

# -- machine context decision (mirrors configure.ac) ---------------------------
set(_mcsc_works FALSE)
if(HAVE_UCONTEXT_H AND HAVE_MAKECONTEXT AND HAVE_SWAPCONTEXT
   AND HAVE_GETCONTEXT AND HAVE_SETCONTEXT)
    if(CMAKE_CROSSCOMPILING)
        set(_mcsc_works TRUE)   # assume usable, like most modern targets
    else()
        try_run(_run _compile "${CMAKE_CURRENT_BINARY_DIR}/probes" "${_probes}/mcsc.c")
        if(_compile AND _run EQUAL 0)
            set(_mcsc_works TRUE)
        endif()
    endif()
endif()

set(_mth "${PTH_MCTX_MTH}")
set(_dsp "${PTH_MCTX_DSP}")
set(_stk "${PTH_MCTX_STK}")
if(_mth STREQUAL "")
    if(_mcsc_works)
        set(_mth mcsc)
    elseif(HAVE_SIGSETJMP OR HAVE_SETJMP)
        set(_mth sjlj)
    else()
        message(FATAL_ERROR "no appropriate mctx method found")
    endif()
endif()
if(_dsp STREQUAL "")
    if(_mth STREQUAL "mcsc")
        set(_dsp sc)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_dsp sjljlx)
    elseif(HAVE_SIGSETJMP)
        set(_dsp ssjlj)
    elseif(HAVE__SETJMP)
        set(_dsp usjlj)
    else()
        set(_dsp sjlj)
    endif()
endif()
if(_stk STREQUAL "")
    if(_mth STREQUAL "mcsc")
        set(_stk mc)
    elseif(_dsp MATCHES "^(sjljlx|sjljisc|sjljw32)$")
        set(_stk none)
    elseif(HAVE_SIGALTSTACK)
        set(_stk sas)
    elseif(HAVE_SIGSTACK)
        set(_stk ss)
    else()
        set(_stk none)
    endif()
endif()
set(PTH_MCTX_MTH_RESOLVED "${_mth}")
set(PTH_MCTX_DSP_RESOLVED "${_dsp}")
set(PTH_MCTX_STK_RESOLVED "${_stk}")
message(STATUS "Pth mctx implementation: ${_mth}/${_dsp}/${_stk}")

# -- multi-scheduler (MP) requirements ----------------------------------------
if(PTH_MP)
    # the sjlj mctx variants bootstrap stacks via process-global state and a
    # SIGUSR1 handler, which is unsafe with concurrent schedulers (see
    # MULTISCHED-DESIGN.md section 7): require the reentrant makecontext variant
    if(NOT _mth STREQUAL "mcsc")
        message(FATAL_ERROR "PTH_MP requires the mcsc (makecontext/swapcontext) "
                            "mctx method; detected/forced: ${_mth}/${_dsp}/${_stk}")
    endif()
    check_c_source_compiles("
        #if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
        #include <stdatomic.h>
        static atomic_int x;
        #elif defined(__GNUC__) || defined(__clang__)
        static int x;
        #else
        #error no atomics
        #endif
        static __thread int t;
        int main(void) { t = 1; return 0; }"
        PTH_MP_TOOLCHAIN_OK)
    if(NOT PTH_MP_TOOLCHAIN_OK)
        message(FATAL_ERROR "PTH_MP requires C11 atomics or GCC/Clang __atomic "
                            "builtins plus __thread TLS support")
    endif()
    find_package(Threads REQUIRED)
endif()

# sig{set,long}jmp flavor (for pth_acmac.h)
if(_dsp MATCHES "^(ssjlj|sjljlx|sjljisc|sjljw32)$" OR (_dsp STREQUAL "sc" AND HAVE_SIGSETJMP))
    set(pth_sigjmpbuf  "#define pth_sigjmpbuf sigjmp_buf")
    set(pth_sigsetjmp  "#define pth_sigsetjmp(buf) sigsetjmp(buf,1)")
    set(pth_siglongjmp "#define pth_siglongjmp(buf,val) siglongjmp(buf,val)")
elseif(_dsp STREQUAL "usjlj")
    set(pth_sigjmpbuf  "#define pth_sigjmpbuf jmp_buf")
    set(pth_sigsetjmp  "#define pth_sigsetjmp(buf) _setjmp(buf)")
    set(pth_siglongjmp "#define pth_siglongjmp(buf,val) _longjmp(buf,val)")
else()
    set(pth_sigjmpbuf  "#define pth_sigjmpbuf jmp_buf")
    set(pth_sigsetjmp  "#define pth_sigsetjmp(buf) setjmp(buf)")
    set(pth_siglongjmp "#define pth_siglongjmp(buf,val) longjmp(buf,val)")
endif()

# stack setup macros (for pth_acmac.h); probe prints "skaddr-expr,sksize-expr"
macro(pth_stacksetup_split name raw)
    string(REGEX MATCH "^(.*),([^,]*)$" _m "${raw}")
    set(pth_skaddr_${name} "#define pth_skaddr_${name}(skaddr,sksize) (${CMAKE_MATCH_1})")
    set(pth_sksize_${name} "#define pth_sksize_${name}(skaddr,sksize) (${CMAKE_MATCH_2})")
endmacro()

macro(pth_stacksetup2 name testdef)
    set(_defs "-D${testdef}")
    if(HAVE_STACK_T)
        list(APPEND _defs "-DHAVE_STACK_T")
    endif()
    pth_run_probe("${_probes}/stacksetup.c" _setup "(skaddr),(sksize)" ${_defs})
    if(NOT _setup MATCHES "^\\(skaddr\\)")
        set(_setup "(skaddr),(sksize)")
    endif()
    pth_stacksetup_split(${name} "${_setup}")
endmacro()

if(HAVE_MAKECONTEXT)
    pth_stacksetup2(makecontext TEST_makecontext)
else()
    pth_stacksetup_split(makecontext "(skaddr),(sksize)")
endif()
if(HAVE_SIGALTSTACK)
    pth_stacksetup2(sigaltstack TEST_sigaltstack)
else()
    pth_stacksetup_split(sigaltstack "(skaddr),(sksize)")
endif()
if(HAVE_SIGSTACK)
    pth_stacksetup2(sigstack TEST_sigstack)
else()
    pth_stacksetup_split(sigstack "(skaddr),(sksize)")
endif()

# -- remaining substitution variables ------------------------------------------
if(HAVE_SYS_SELECT_H)
    set(EXTRA_INCLUDE_SYS_SELECT_H "#include <sys/select.h>")
else()
    set(EXTRA_INCLUDE_SYS_SELECT_H "/* #include <sys/select.h> */")
endif()

set(PTH_EXT_SFIO 0)   # Sfio/OSSP-ex/Dmalloc integration not supported by CMake build

if(PTH_SYSCALL_SOFT)
    set(PTH_SYSCALL_SOFT 1)
else()
    set(PTH_SYSCALL_SOFT 0)
endif()
if(PTH_SYSCALL_HARD AND HAVE_SYSCALL AND HAVE_SYS_SYSCALL_H AND HAVE_SYS_READ)
    set(PTH_SYSCALL_HARD 1)
else()
    set(PTH_SYSCALL_HARD 0)
endif()

set(PTH_SYSCALL_LIBS "" CACHE STRING "Colon-separated libc paths for hard syscall mapping")

# -- generate headers -----------------------------------------------------------
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")

# pth_acdef.h (autoheader equivalent)
set(PTH_MCTX_MTH "${PTH_MCTX_MTH_RESOLVED}")
set(PTH_MCTX_DSP "${PTH_MCTX_DSP_RESOLVED}")
set(PTH_MCTX_STK "${PTH_MCTX_STK_RESOLVED}")
configure_file(cmake/pth_acdef.h.cmake.in "${GEN}/pth_acdef.h" @ONLY)

# pth_acmac.h, pth.h, pthread.h from the original autoconf templates
configure_file(pth_acmac.h.in "${GEN}/pth_acmac.h" @ONLY)
configure_file(pth.h.in       "${GEN}/pth.h"       @ONLY)
if(PTH_BUILD_PTHREAD)
    configure_file(pthread.h.in "${GEN}/pthread.h" @ONLY)
endif()

# pth_p.h via shtool scpp (extracts `#if cpp` sections of the sources)
set(PTH_HSRCS
    pth_compat.c pth_debug.c pth_syscall.c pth_errno.c pth_ring.c pth_mctx.c
    pth_uctx.c pth_clean.c pth_time.c pth_tcb.c pth_util.c pth_pqueue.c
    pth_event.c pth_sched.c pth_data.c pth_msg.c pth_cancel.c pth_sync.c
    pth_attr.c pth_lib.c pth_fork.c pth_high.c pth_ext.c pth_string.c pthread.c)
set(_hsrc_paths "")
foreach(f ${PTH_HSRCS})
    list(APPEND _hsrc_paths "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
endforeach()
find_program(SH_PROGRAM sh REQUIRED)
add_custom_command(
    OUTPUT "${GEN}/pth_p.h"
    COMMAND ${SH_PROGRAM} "${CMAKE_CURRENT_SOURCE_DIR}/shtool" scpp
            -o "${GEN}/pth_p.h" -t "${CMAKE_CURRENT_SOURCE_DIR}/pth_p.h.in"
            -Dcpp -Cintern -M "==#==" ${_hsrc_paths}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pth_p.h.in" ${_hsrc_paths}
            "${CMAKE_CURRENT_SOURCE_DIR}/shtool"
            "${CMAKE_CURRENT_SOURCE_DIR}/pth_atomic.h"
    COMMENT "Generating pth_p.h (shtool scpp)"
    VERBATIM)
add_custom_target(pth_p_h DEPENDS "${GEN}/pth_p.h")

# -- library -----------------------------------------------------------------
set(PTH_SOURCES
    pth_debug.c pth_ring.c pth_pqueue.c pth_time.c pth_errno.c pth_mctx.c
    pth_uctx.c pth_tcb.c pth_sched.c pth_attr.c pth_lib.c pth_event.c
    pth_data.c pth_clean.c pth_cancel.c pth_msg.c pth_sync.c pth_fork.c
    pth_util.c pth_high.c pth_syscall.c pth_ext.c pth_compat.c pth_string.c)

add_library(pth ${PTH_SOURCES})
add_dependencies(pth pth_p_h)
target_include_directories(pth PUBLIC "${GEN}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pth PUBLIC ${PTH_EXTRA_LIBS})
if(PTH_MP)
    target_compile_definitions(pth PUBLIC PTH_MP)
    target_link_libraries(pth PUBLIC Threads::Threads)
endif()
if(PTH_SCHED_POLL)
    target_compile_definitions(pth PRIVATE PTH_SCHED_POLL)
endif()
set_target_properties(pth PROPERTIES
    VERSION "${PTH_VERSION_SHORT}"
    POSITION_INDEPENDENT_CODE ON)

if(PTH_BUILD_PTHREAD)
    add_library(pth_pthread ${PTH_SOURCES} pthread.c)
    add_dependencies(pth_pthread pth_p_h)
    target_include_directories(pth_pthread PUBLIC "${GEN}" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(pth_pthread PRIVATE PTH_EMULATION)
    target_link_libraries(pth_pthread PUBLIC ${PTH_EXTRA_LIBS} ${CMAKE_DL_LIBS})
    if(PTH_MP)
        target_compile_definitions(pth_pthread PUBLIC PTH_MP)
        target_link_libraries(pth_pthread PUBLIC Threads::Threads)
    endif()
    if(PTH_SCHED_POLL)
        target_compile_definitions(pth_pthread PRIVATE PTH_SCHED_POLL)
    endif()
    set_target_properties(pth_pthread PROPERTIES
        OUTPUT_NAME pthread
        VERSION "${PTH_VERSION_SHORT}"
        POSITION_INDEPENDENT_CODE ON)
endif()

# -- tests ---------------------------------------------------------------------
if(PTH_BUILD_TESTS)
    enable_testing()
    foreach(t test_std test_mp test_misc test_philo test_sig
              test_select test_httpd test_uctx test_sync test_mutexfair test_pollfd)
        add_executable(${t} ${t}.c test_common.c)
        target_link_libraries(${t} PRIVATE pth)
    endforeach()
    if(PTH_BUILD_PTHREAD)
        add_executable(test_pthread test_pthread.c test_common.c)
        target_link_libraries(test_pthread PRIVATE pth_pthread)
        if(PTH_MP)
            # verifies pthread_create(3) spreads threads across schedulers
            add_executable(test_pthread_mp test_pthread_mp.c)
            target_link_libraries(test_pthread_mp PRIVATE pth_pthread)
            add_test(NAME test_pthread_mp COMMAND test_pthread_mp)
            set_tests_properties(test_pthread_mp PROPERTIES TIMEOUT 120)
        endif()
    endif()
    # Only test_std and test_uctx are self-terminating, non-interactive tests;
    # the others (mp, sig, philo, misc, select, httpd) are interactive demos.
    foreach(t test_std test_uctx test_sync test_mutexfair test_pollfd)
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 120)
    endforeach()
    if(PTH_MP)
        foreach(t test_mpsched test_mutexfair_mp test_join_mp test_msgport_mp test_cancelraise_mp test_suspend_mp test_cancelblock_mp test_once_mp)
            add_executable(${t} ${t}.c test_common.c)
            target_link_libraries(${t} PRIVATE pth)
            add_test(NAME ${t} COMMAND ${t})
            set_tests_properties(${t} PROPERTIES TIMEOUT 120)
        endforeach()
    endif()
endif()

# -- install --------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS pth
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES "${GEN}/pth.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(PTH_BUILD_PTHREAD)
    install(TARGETS pth_pthread
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${GEN}/pthread.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pth)
endif()
