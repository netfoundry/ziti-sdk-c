cmake_minimum_required (VERSION 3.12)
file(READ version ver)

project (ziti-sdk
        VERSION ${ver}
        LANGUAGES C CXX
        )

message("project version: ${PROJECT_VERSION}")

if (WIN32)
    message("WIN32 build. Creating: ${CMAKE_BINARY_DIR}/cmake_install")
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/cmake_install)
    message("WIN32 build. Creating: ${CMAKE_BINARY_DIR}/cmake_install/ziti-sdk-${PROJECT_VERSION}")
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/cmake_install/ziti-sdk-${PROJECT_VERSION})
    set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/cmake_install/ziti-sdk-${PROJECT_VERSION})
else()
    set(CMAKE_INSTALL_PREFIX /opt/netfoundry/ziti-sdk-${PROJECT_VERSION})
endif()

message("cross-compiling ${CMAKE_CROSSCOMPILING}")

enable_testing()

# Get the current working branch
execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get the latest abbreviated commit hash of the working branch
execute_process(
        COMMAND git log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

# capture build date
execute_process(
        COMMAND date +%a-%m/%d/%Y-%H:%M:%S-%Z
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE BUILD_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

message("git info:")
message("   branch : ${GIT_BRANCH}")
message("     hash : ${GIT_COMMIT_HASH}")
message("     date : ${BUILD_DATE}")

if (DEFINED ENV{BITBUCKET_BUILD_NUMBER})
    set(ZITI_BUILDNUM $ENV{BITBUCKET_BUILD_NUMBER})
else ()
    set(ZITI_BUILDNUM local)
endif ()

set(ZITI_VERSION ${PROJECT_VERSION}-${ZITI_BUILDNUM})

link_directories(${CMAKE_BINARY_DIR}/lib)

add_subdirectory(deps)

if(WIN32)
  add_subdirectory(windows)
endif()

set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY 0)
set(CPACK_PACKAGE_VERSION ${ZITI_VERSION})
set(CPACK_OUTPUT_FILE_PREFIX ${CMAKE_CURRENT_BINARY_DIR})
if (UNIX)
    set(CPACK_GENERATOR "TGZ")
elseif (WIN32)
    set(CPACK_GENERATOR "ZIP")
endif ()

include(CPack)
add_custom_target(publish)

if(${GIT_BRANCH} STREQUAL master)
    set(PUB_PFX ziti-staging)
else()
    set(PUB_PFX ziti-snapshot/${GIT_BRANCH})
endif()

set(PUB_OPTS --apikey=$ENV{JFROG_API_KEY} --url https://netfoundry.jfrog.io/netfoundry --build-name=ziti --build-number=${ZITI_VERSION})
string(TOLOWER ${CMAKE_SYSTEM_NAME} PUB_OS)
string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} PUB_ARCH)

if(${PUB_ARCH} STREQUAL "x86_64" )
    set(PUB_ARCH "amd64")
endif()

macro(PUBCOMP comp)
    if(NOT WIN32)
    set(${comp}_PUB_PATH ${PUB_PFX}/${comp}/${PUB_ARCH}/${PUB_OS}/${ZITI_VERSION}/${comp}.tar.gz)
    set(${comp}_PUB_PROPS "version=${ZITI_VERSION}\;name=${comp}\;arch=${PUB_ARCH}\;os=${PUB_OS}\;branch=${GIT_BRANCH}")

    add_custom_target(${comp}-pub
            DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${comp}-${ZITI_VERSION}-${CMAKE_SYSTEM_NAME}.tar.gz
            COMMAND jfrog rt u ${PUB_OPTS} --props='${${comp}_PUB_PROPS}' ${comp}-${ZITI_VERSION}-${CMAKE_SYSTEM_NAME}.tar.gz ${${comp}_PUB_PATH}
            )

    add_dependencies(publish ${comp}-pub)
    endif()
endmacro(PUBCOMP)

# use prefix length to trim path for logging, see inc_internal/utils.h __FILENAME__
string(LENGTH "${CMAKE_SOURCE_DIR}/" SOURCE_PATH_SIZE)
add_definitions("-DSOURCE_PATH_SIZE=${SOURCE_PATH_SIZE}")

add_subdirectory(library)

if(NOT ${PUB_OS} STREQUAL "ios" )
    add_subdirectory(programs)
    add_subdirectory(tests)
endif()

PUBCOMP(ziti-sdk)
