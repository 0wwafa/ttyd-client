cmake_minimum_required(VERSION 3.12.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(ttyd-client VERSION 1.0.0 LANGUAGES C)

set(TTYD_CLIENT_VERSION "${PROJECT_VERSION}")

include(GetGitVersion)
get_git_version(GIT_VERSION SEM_VER)
get_git_head(GIT_COMMIT)
if("${SEM_VER}" VERSION_GREATER "${TTYD_CLIENT_VERSION}")
    set(TTYD_CLIENT_VERSION "${SEM_VER}")
endif()
if(NOT "${GIT_COMMIT}" STREQUAL "" AND NOT "${GIT_COMMIT}" STREQUAL "unknown")
    set(TTYD_CLIENT_VERSION "${TTYD_CLIENT_VERSION}-${GIT_COMMIT}")
endif()

if(NOT MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")
    set(CMAKE_C_STANDARD 99)
else()
    # MSVC: suppress common warnings for C99 code
    add_compile_options(/W3 /wd4996 /wd4267 /wd4244)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _GNU_SOURCE)
endif()

option(TC_WITH_TLS "Enable TLS (wss/https) support with OpenSSL" ON)
option(TC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(SOURCE_FILES
    src/b64.c
    src/buffer.c
    src/client.c
    src/http.c
    src/io.c
    src/json.c
    src/log.c
    src/main.c
    src/net.c
    src/protocol.c
    src/sha1.c
    src/term.c
    src/tls.c
    src/url.c
    src/utils.c
    src/ws.c
)

set(LINK_LIBS "")

if(TC_WITH_TLS)
    find_package(OpenSSL)
    if(OPENSSL_FOUND)
        list(APPEND INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
        # Prefer the imported targets: they carry the dependency information
        # (eg: a statically linked zlib) instead of raw variables that may not
        # be resolvable when cross compiling.
        if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
            list(APPEND LINK_LIBS OpenSSL::SSL OpenSSL::Crypto)
        else()
            list(APPEND LINK_LIBS ${OPENSSL_LIBRARIES})
        endif()
        add_definitions(-DTC_WITH_TLS=1)
        message(STATUS "TLS support: enabled (OpenSSL ${OPENSSL_VERSION})")
    else()
        message(WARNING "OpenSSL not found, building without TLS support (ws:// only)")
    endif()
else()
    message(STATUS "TLS support: disabled")
endif()

if(WIN32)
    # winsock2, and the static libuv style dependencies are not needed here
    list(APPEND LINK_LIBS ws2_32)
    if(MSVC)
        find_package(getopt CONFIG)
        if(getopt_FOUND)
            if(TARGET getopt::getopt_shared)
                list(APPEND LINK_LIBS getopt::getopt_shared)
            elseif(TARGET getopt::getopt_static)
                list(APPEND LINK_LIBS getopt::getopt_static)
            endif()
        endif()
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    list(APPEND SOURCE_FILES ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
endif()

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${INCLUDE_DIRS} src)
if(WIN32)
    # BCryptGenRandom, used for the websocket masking keys
    list(APPEND LINK_LIBS bcrypt)
endif()

target_link_libraries(${PROJECT_NAME} ${LINK_LIBS})
target_compile_definitions(${PROJECT_NAME} PUBLIC
    TTYD_CLIENT_VERSION="${TTYD_CLIENT_VERSION}"
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0xa00 WINVER=0xa00>
)

if(NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(TC_WARNINGS_AS_ERRORS)
        target_compile_options(${PROJECT_NAME} PRIVATE -Werror)
    endif()
endif()

include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT prog)
install(FILES man/ttyd-client.1 DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT doc)
