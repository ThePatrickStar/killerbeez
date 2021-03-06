cmake_minimum_required (VERSION 2.8.8)
project (killerbeez)
include(ExternalProject)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_DEBUG")	# for _DEBUG ifdefs in utils.h

if (UNIX)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wfatal-errors") # quit after first error
endif (UNIX)

if (APPLE)
	set(CMAKE_MACOSX_RPATH 1) # https://github.com/liballeg/allegro5/issues/532#issuecomment-170338164
endif (APPLE)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug) 	# for gcc -g
	message("WARNING: Building with debug options; performance will be impacted. Try cmake -DCMAKE_BUILD_TYPE=Release ../killerbeez")
endif()


if (WIN32)
	# windows/visual studio build convention eg build/X86/Debug
	SET ( BUILD_DIRECTORY ${CMAKE_SOURCE_DIR}/../build/${CMAKE_C_COMPILER_ARCHITECTURE_ID}/${CMAKE_BUILD_TYPE} )
	add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_DEPRECATION_DISABLE -D_CRT_NONSTDC_NO_DEPRECATE)
else (WIN32)
	SET ( BUILD_DIRECTORY ${CMAKE_BINARY_DIR} )
endif (WIN32)

SET( CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BUILD_DIRECTORY}/killerbeez/ )
SET( CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BUILD_DIRECTORY}/killerbeez/ )
SET( CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BUILD_DIRECTORY}/killerbeez/ )
LINK_DIRECTORIES(${BUILD_DIRECTORY}/killerbeez/) # give linker path to utils

# Make linux builds relocatable
SET(CMAKE_BUILD_WITH_INSTALL_RPATH 1)
SET(CMAKE_INSTALL_RPATH "$ORIGIN")

# add headers for utils from utils source folder/repo
include_directories (${CMAKE_SOURCE_DIR}/../killerbeez-utils/jansson/)
include_directories (${CMAKE_SOURCE_DIR}/../killerbeez-utils/utils/)

# compile mutators, which will compile utils
add_subdirectory(${CMAKE_SOURCE_DIR}/../killerbeez-mutators/ ${CMAKE_BINARY_DIR}/killerbeez-mutators/)

add_subdirectory(corpus) # test programs
add_subdirectory(fuzzer) # instantiates & coordinates other parts
add_subdirectory(driver) # starts program, feeds input, determines when program is done
add_subdirectory(instrumentation) # inserts instructions to program to tell whether an input makes the binary take a new path
add_subdirectory(merger) # merges instrumentation data between fuzzer nodes
add_subdirectory(tracer) # runs through program and records basic block edges

if (WIN32)
add_subdirectory(picker) # picks which libraries of a target program are being used, and worth fuzzing
add_subdirectory(winafl) # parts ripped from winafl for dynamorio
endif (WIN32)

### RELEASE ZIP CONFIG ###
# Choose what to install into the release zip
install(DIRECTORY ${BUILD_DIRECTORY}/killerbeez DESTINATION . USE_SOURCE_PERMISSIONS)
install(DIRECTORY ${BUILD_DIRECTORY}/mutators DESTINATION . USE_SOURCE_PERMISSIONS)

### BOINC wrapper ###
# If on Windows, include prebuilt BOINC wrapper
if (WIN32)
  SET(BOINC_WRAPPER C:/killerbeez/wrapper_26014_windows_x86_64.exe)
endif ()
# If on Linux, we can build our own BOINC wrapper
if (UNIX)
  SET(BOINC_WRAPPER ${CMAKE_SOURCE_DIR}/server/boinc/samples/wrapper/wrapper)
  ExternalProject_Add(boinc-wrapper
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/server/boinc
    DOWNLOAD_COMMAND cd <SOURCE_DIR> && git submodule update --init .
    BUILD_IN_SOURCE true
    CONFIGURE_COMMAND ./_autosetup
    COMMAND ./configure --disable-server --disable-client --disable-manager --enable-boinczip
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C samples/wrapper
    INSTALL_COMMAND ""
    EXCLUDE_FROM_ALL true
  )
endif ()
install(PROGRAMS ${BOINC_WRAPPER} DESTINATION server/skel)

### radamsa ###
set(RADAMSA_URL https://gitlab.com/akihe/radamsa.git)
ExternalProject_Add(radamsa
  GIT_REPOSITORY ${RADAMSA_URL}
  BUILD_IN_SOURCE true
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
  INSTALL_COMMAND ""
  EXCLUDE_FROM_ALL true
)
ExternalProject_Get_Property(radamsa SOURCE_DIR)
install(DIRECTORY ${SOURCE_DIR}/bin DESTINATION radamsa USE_SOURCE_PERMISSIONS)
install(FILES ${SOURCE_DIR}/LICENCE DESTINATION radamsa)

# Set up CPack to generate the release zip
SET(CPACK_GENERATOR "ZIP")
# TODO: might want some kind of versioning or architecture in this name
SET(CPACK_PACKAGE_FILE_NAME "killerbeez-${CMAKE_SYSTEM_NAME}")
SET(CPACK_SOURCE_GENERATOR "ZIP")
SET(CPACK_SOURCE_IGNORE_FILES "/build/;/server/boinc/")
SET(CPACK_SOURCE_INSTALLED_DIRECTORIES "${CMAKE_SOURCE_DIR};killerbeez;${CMAKE_SOURCE_DIR}/../killerbeez-mutators;killerbeez-mutators;${CMAKE_SOURCE_DIR}/../killerbeez-utils;killerbeez-utils")
include (CPack)

# Special `release` target to ensure boinc-wrapper is built before package
add_custom_target(release
  ${CMAKE_MAKE_PROGRAM} package)
add_dependencies(release radamsa boinc-wrapper)
