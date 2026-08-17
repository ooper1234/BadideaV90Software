cmake_minimum_required(VERSION 3.16)
project(v92isp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(V92_CORE_SOURCES
    src/g711.cpp
    src/v21.cpp
    src/v23.cpp
    src/async_serial.cpp
    src/answer_tones.cpp
    src/modem_modes.cpp
    src/v92_quickconnect.cpp
    src/v90_phase2.cpp
    src/v90_phase3.cpp
    src/v90_phase3_rx.cpp
    src/v90_pcm.cpp
    src/v34_qam.cpp
    src/ppp_userspace.cpp
    src/sip_rtp.cpp
    src/live_modem.cpp
    src/span_v22.cpp
    src/span_v8.cpp
)
if(NOT WIN32)
    list(APPEND V92_CORE_SOURCES src/ppp_backend.cpp)
endif()

add_library(v92core ${V92_CORE_SOURCES})
target_include_directories(v92core PUBLIC include)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SPANDSP QUIET IMPORTED_TARGET spandsp)
endif()

# Cross-platform protocol regressions. The historical all-in-one test includes
# the Unix pppd backend and therefore could not run in the shipped Windows build.
add_executable(v92_protocol_tests tests/protocol_test.cpp)
target_link_libraries(v92_protocol_tests PRIVATE v92core)
enable_testing()
add_test(NAME v92_protocol_tests COMMAND v92_protocol_tests)
if(SPANDSP_FOUND)
    target_compile_definitions(v92core PUBLIC V92_HAVE_SPANDSP=1)
    target_link_libraries(v92core PUBLIC PkgConfig::SPANDSP)
    message(STATUS "SpanDSP found: V.8 + live V.22/V.22bis enabled")
else()
    message(STATUS "SpanDSP not found: V.22/V.22bis optional live PHY disabled")
endif()

find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_compile_definitions(v92core PUBLIC V92_HAVE_OPENSSL=1)
    target_link_libraries(v92core PUBLIC OpenSSL::Crypto)
    message(STATUS "OpenSSL found: SIP Digest REGISTER enabled")
elseif(PkgConfig_FOUND)
    pkg_check_modules(OPENSSL_PC QUIET IMPORTED_TARGET openssl)
    if(OPENSSL_PC_FOUND)
        target_compile_definitions(v92core PUBLIC V92_HAVE_OPENSSL=1)
        target_link_libraries(v92core PUBLIC PkgConfig::OPENSSL_PC)
        message(STATUS "OpenSSL found via pkg-config: SIP Digest REGISTER enabled")
    else()
        message(STATUS "OpenSSL not found: SIP REGISTER works only without Digest authentication")
    endif()
else()
    message(STATUS "OpenSSL not found: SIP REGISTER works only without Digest authentication")
endif()

if(WIN32)
    target_compile_definitions(v92core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(v92core PUBLIC ws2_32)
    add_executable(v92isp-windows
        src/windows_main.cpp
        src/wintun_adapter.cpp
    )
    target_compile_definitions(v92isp-windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(v92isp-windows PRIVATE v92core ws2_32 iphlpapi)
    # Make the MSYS2 build easier to redistribute; SpanDSP/OpenSSL DLLs are
    # copied by windows/Build-Windows.ps1, while the C++ runtimes are static.
    if(MINGW)
        target_link_options(v92isp-windows PRIVATE -static-libgcc -static-libstdc++)
    endif()

    if(PkgConfig_FOUND)
        pkg_check_modules(PJPROJECT QUIET libpjproject)
        if(PJPROJECT_FOUND)
            add_executable(v92isp-windows-pjsip
                src/windows_pjsip_main.cpp
                src/pjsip_frontend.cpp
                src/wintun_adapter.cpp
                src/windivert_nat.cpp
            )
            target_compile_definitions(v92isp-windows-pjsip PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
            if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/windivert/include/windivert.h")
                target_include_directories(v92isp-windows-pjsip PRIVATE "${CMAKE_SOURCE_DIR}/third_party/windivert/include")
                target_compile_definitions(v92isp-windows-pjsip PRIVATE V92_HAVE_WINDIVERT=1)
                message(STATUS "WinDivert header found: user-mode NAT fallback enabled")
            else()
                message(WARNING "WinDivert header missing: user-mode NAT fallback will not build")
            endif()
            # pkg-config under MSYS2 can report /c/... paths while Ninja launches
            # the native MinGW compiler, which needs C:/... paths. The installer
            # exports V92_PJSIP_PREFIX in native Windows form; prefer it here.
            if(DEFINED ENV{V92_PJSIP_PREFIX} AND NOT "$ENV{V92_PJSIP_PREFIX}" STREQUAL "")
                file(TO_CMAKE_PATH "$ENV{V92_PJSIP_PREFIX}" V92_PJSIP_PREFIX_NATIVE)
                target_include_directories(v92isp-windows-pjsip PRIVATE "${V92_PJSIP_PREFIX_NATIVE}/include")
                target_link_directories(v92isp-windows-pjsip PRIVATE "${V92_PJSIP_PREFIX_NATIVE}/lib")
            endif()
            target_include_directories(v92isp-windows-pjsip PRIVATE ${PJPROJECT_INCLUDE_DIRS})
            target_link_directories(v92isp-windows-pjsip PRIVATE ${PJPROJECT_LIBRARY_DIRS})

            # libpjproject.pc lists -lstdc++ because PJSUA2 is C++. Since this
            # target is already linked by the C++ driver, and on MinGW we ask
            # the driver for -static-libstdc++, passing PJSIP's explicit
            # -lstdc++ as well mixes libstdc++.dll.a with libstdc++.a and causes
            # hundreds of duplicate C++ runtime definitions. Let c++ add its
            # runtime exactly once at the end of the link instead.
            set(V92_PJPROJECT_STATIC_LIBRARIES ${PJPROJECT_STATIC_LIBRARIES})
            list(FILTER V92_PJPROJECT_STATIC_LIBRARIES EXCLUDE REGEX "^stdc[+][+]$")
            set(V92_PJPROJECT_STATIC_LDFLAGS_OTHER ${PJPROJECT_STATIC_LDFLAGS_OTHER})
            list(FILTER V92_PJPROJECT_STATIC_LDFLAGS_OTHER EXCLUDE REGEX "^-lstdc[+][+]$")

            target_link_libraries(v92isp-windows-pjsip PRIVATE v92core ${V92_PJPROJECT_STATIC_LIBRARIES} ws2_32 iphlpapi ole32 winmm)
            target_link_options(v92isp-windows-pjsip PRIVATE ${V92_PJPROJECT_STATIC_LDFLAGS_OTHER})
            if(MINGW)
                target_link_options(v92isp-windows-pjsip PRIVATE -static-libgcc -static-libstdc++)
            endif()

            # Exercise the exact encoder table linked into the shipping PJSIP
            # binary. The portable test covers the bridge helper; this catches
            # a missing dependency patch that would corrupt V.90 Sd/S-bar.
            add_executable(v92_pjsip_pcmu_tests tests/pjsip_pcmu_test.cpp)
            target_compile_definitions(v92_pjsip_pcmu_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
            if(DEFINED V92_PJSIP_PREFIX_NATIVE)
                target_include_directories(v92_pjsip_pcmu_tests PRIVATE "${V92_PJSIP_PREFIX_NATIVE}/include")
                target_link_directories(v92_pjsip_pcmu_tests PRIVATE "${V92_PJSIP_PREFIX_NATIVE}/lib")
            endif()
            target_include_directories(v92_pjsip_pcmu_tests PRIVATE ${PJPROJECT_INCLUDE_DIRS})
            target_link_directories(v92_pjsip_pcmu_tests PRIVATE ${PJPROJECT_LIBRARY_DIRS})
            target_link_libraries(v92_pjsip_pcmu_tests PRIVATE v92core ${V92_PJPROJECT_STATIC_LIBRARIES} ws2_32 iphlpapi ole32 winmm)
            target_link_options(v92_pjsip_pcmu_tests PRIVATE ${V92_PJPROJECT_STATIC_LDFLAGS_OTHER})
            if(MINGW)
                target_link_options(v92_pjsip_pcmu_tests PRIVATE -static-libgcc -static-libstdc++)
            endif()
            add_test(NAME v92_pjsip_pcmu_tests COMMAND v92_pjsip_pcmu_tests)
            message(STATUS "PJSIP found: mature SIP/RTP Windows frontend enabled")
        else()
            message(STATUS "PJSIP not found: mature frontend disabled")
        endif()
    endif()
else()
    add_executable(v92lab src/main.cpp)
    target_link_libraries(v92lab PRIVATE v92core)

    add_executable(v92isp-ppp-bridge src/ppp_bridge_main.cpp)
    target_link_libraries(v92isp-ppp-bridge PRIVATE v92core)

    add_executable(v92isp-ppp-tun src/ppp_tun_main.cpp)
    target_link_libraries(v92isp-ppp-tun PRIVATE v92core)

    add_executable(v92isp-live src/live_main.cpp)
    target_link_libraries(v92isp-live PRIVATE v92core)

    add_executable(v92_tests tests/test_main.cpp)
    target_link_libraries(v92_tests PRIVATE v92core)

    add_executable(v92_register_selftest tests/register_selftest.cpp)
    target_link_libraries(v92_register_selftest PRIVATE v92core)

    enable_testing()
    add_test(NAME v92_tests COMMAND v92_tests)
    add_test(NAME v92_register_selftest COMMAND v92_register_selftest)
    add_test(NAME v92_userspace_ppp_selftest COMMAND v92isp-ppp-tun --selftest)
endif()
