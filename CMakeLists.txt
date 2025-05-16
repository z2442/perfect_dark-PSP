cmake_minimum_required(VERSION 3.16)
project(pd C CXX) # <<< MOVED THIS LINE UP

# --- PSP Build Configuration ---
# To build for PSP, invoke CMake with:
# cmake -S . -B build-psp -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/psp.toolchain.cmake -DBUILD_PSP=ON [other options]
# where psp.toolchain.cmake points to your PSP toolchain setup.
option(BUILD_PSP "Build for PlayStation Portable" OFF)

if(BUILD_PSP)
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(WARNING "Building for PSP (BUILD_PSP=ON) but CMAKE_TOOLCHAIN_FILE is not set. "
                    "Cross-compilation might fail unless the toolchain is in your PATH and CMake detects it correctly. "
                    "It is strongly recommended to use a toolchain file (e.g., cmake/psp.toolchain.cmake).")
  endif()
  # PSP environment checks (optional but helpful)
  if(NOT DEFINED ENV{PSPSDK})
      message(WARNING "PSPSDK environment variable is not set. Assuming PSP development libraries are findable in standard paths.")
      set(PSPSDK "" CACHE PATH "Path to the PSPSDK root directory")
  else()
      set(PSPSDK "$ENV{PSPSDK}" CACHE PATH "Path to the PSPSDK root directory")
      message(STATUS "Using PSPSDK: ${PSPSDK}")
      # Add PSP SDK paths for finding headers and libraries if the toolchain file doesn't
      list(APPEND CMAKE_SYSTEM_INCLUDE_PATH "${PSPSDK}/psp/sdk/include" "${PSPSDK}/psp/include")
      list(APPEND CMAKE_SYSTEM_LIBRARY_PATH "${PSPSDK}/psp/sdk/lib" "${PSPSDK}/psp/lib")
      # Make sure pkg-config uses the PSP paths if needed
      set(ENV{PKG_CONFIG_PATH} "${PSPSDK}/psp/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
      set(ENV{PKG_CONFIG_LIBDIR} "${PSPSDK}/psp/lib:$ENV{PKG_CONFIG_LIBDIR}")
  endif()

  # Disable macOS specific settings when building for PSP
  set(APPLE OFF)
  set(UNIX OFF) # Usually set by CMake even for PSP, override if necessary for logic below
  set(WIN32 OFF)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# --- macOS Deployment Target (Only if actually building for macOS) ---
if(APPLE AND NOT BUILD_PSP)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum OS X deployment version")
  endif()
endif()

# project(pd C CXX) # <<< REMOVED FROM HERE

include(TargetArch) # May need adjustments for PSP MIPS detection if not handled by toolchain
include(FindPkgConfig)
include(AssetUtils)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20) # Check if psp-g++ supports C++20, might need 17 or 14
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Build Type Default ---
if (NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# --- Options ---
set(ROMID "ntsc-final" CACHE STRING "Which ROM to build an executable for: ntsc-final, pal-final or jpn-final")
# option(FORCE_CROSSCOMPILE "Force cross compiling" NO) # Less relevant when using toolchain file

# --- Target Architecture & OS ---
if(BUILD_PSP)
  set(TARGET_ARCH "psp") # Specific identifier for PSP
  set(TARGET_OS "psp")
  set(TARGET_IS_64BIT FALSE)
  set(TARGET_IS_BIG_ENDIAN FALSE) # PSP is MIPS Little Endian
  set(CMAKE_SYSTEM_NAME "psp" CACHE STRING "System Name for PSP" FORCE) # Help CMake identify PSP
else()
  # Original desktop/switch architecture detection
  target_architecture(TARGET_ARCH)
  if(TARGET_ARCH STREQUAL "i386")
    set(TARGET_ARCH "i686")
  endif()

  # TODO: Handle big endian if supporting other platforms
  set(TARGET_IS_BIG_ENDIAN FALSE)

  if((TARGET_ARCH MATCHES "unknown") OR (TARGET_ARCH STREQUAL ""))
    message(FATAL_ERROR "Unknown target arch")
  elseif(TARGET_ARCH MATCHES "64")
    set(TARGET_IS_64BIT TRUE)
  else()
    set(TARGET_IS_64BIT FALSE)
  endif()

  message(STATUS "Target arch: ${TARGET_ARCH}")

  if(WIN32)
    set(TARGET_OS "windows")
  elseif(APPLE)
    set(TARGET_OS "osx")
  elseif(NINTENDO_SWITCH) # Assuming NINTENDO_SWITCH is set by its toolchain/platform file
    set(TARGET_OS "nswitch")
  elseif(UNIX)
    set(TARGET_OS "linux")
  else()
    message(FATAL_ERROR "Unknown target OS or compiler")
  endif()
endif()

set(TARGET_PLATFORM "${TARGET_ARCH}-${TARGET_OS}")
message(STATUS "Target platform: ${TARGET_PLATFORM}")

# --- Version Info ---
set(VERSION_IS_PAL 0)
if(ROMID STREQUAL "ntsc-final")
  set(VERSION 2)
  set(BIN_REGION "")
elseif(ROMID STREQUAL "pal-final")
  set(VERSION 4)
  set(BIN_REGION ".pal")
  set(VERSION_IS_PAL 1)
elseif(ROMID STREQUAL "jpn-final")
  set(VERSION 5)
  set(BIN_REGION ".jpn")
else()
  message(FATAL_ERROR "Unknown ROM ID: ${ROMID}")
endif()

# Git info (remains the same)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE VERSION_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --abbrev-ref HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE VERSION_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if((VERSION_BRANCH STREQUAL "") OR (VERSION_BRANCH STREQUAL "HEAD"))
  set(VERSION_BRANCH "port-custom")
endif()
if(NOT VERSION_HASH)
  set(VERSION_HASH "unknown")
endif()

set(VERSION_TARGET "${TARGET_PLATFORM}")
set(VERSION_ARCH "${TARGET_ARCH}")
set(VERSION_ROMID "${ROMID}")
set(VERSION_BUILD "${CMAKE_BUILD_TYPE}")
message(STATUS "Version: ${VERSION_ROMID}, hash ${VERSION_HASH}, branch ${VERSION_BRANCH}")
configure_file("${CMAKE_SOURCE_DIR}/port/include/versioninfo.h.in" "${CMAKE_BINARY_DIR}/port/include/versioninfo.h" @ONLY)

# --- Asset/Generated Dirs ---
set(ASSET_DIR "src/assets/${ROMID}")
set(GENERATED_DIR "src/generated/${ROMID}")
set(ENV{ROMID} ${ROMID})

# --- Build Folder Postfix (remains the same) ---
set(BUILD_ID "${VERSION_ROMID}-${VERSION_TARGET}")

# --- Compiler Definitions ---
add_definitions(
  -DMATCHING=0
  -DPIRACYCHECKS=0
  -DROM_SIZE=32
  -DAVOID_UB=1
  -D_LANGUAGE_C=1
  -DPAL=${VERSION_IS_PAL}
  -DVERSION=${VERSION}
  # Add PSP specific defines if needed
  # -DPSP_PLATFORM=1
)
if(TARGET_IS_64BIT) # This will be false for PSP
  add_definitions(-DPLATFORM_64BIT=1)
endif()

# --- Include Directories ---
include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}/include/PR")
include_directories("${CMAKE_SOURCE_DIR}/src/include")
include_directories("${CMAKE_BINARY_DIR}/src/generated/${ROMID}") # Generated headers
include_directories("${CMAKE_SOURCE_DIR}/src/lib/ultra/audio")
include_directories("${CMAKE_SOURCE_DIR}/port/include")
include_directories("${CMAKE_BINARY_DIR}/port/include") # For configured versioninfo.h

# --- Compiler Flags ---

# PSP specific flags (add these *before* common flags if they need to override)
if(BUILD_PSP)
  add_compile_options(
    -G0 # Often required for PSP startup code, handles global pointer. Toolchain might add this automatically.
    -Wall # Keep warnings enabled
    # Add other PSP specific flags as needed, e.g., -fno-builtin-printf for smaller code size if using pspDebugScreenPrintf
  )
  # Optimization: -O3 (optimize for speed) for PSP release builds
  # CMake sets optimization flags per build type, adjust if needed:
  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
      message(STATUS "Using -O3 optimization for PSP ${CMAKE_BUILD_TYPE} build")
      set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
      set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
      set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O3")
      set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O3")
      # Remove potentially problematic desktop optimizations if they were added elsewhere
      # Repeat for CXX and RelWithDebInfo flags...
  endif()

else() # Non-PSP builds
  if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
    add_compile_options(-Og) # Keep original desktop setting
  endif()
  # Platform-specific non-PSP flags
  if (APPLE)
      # Make it look for libs in the executable folder manually since CMake's rpath system is wack
      # Modern CMake way:
      set(CMAKE_INSTALL_RPATH "@executable_path")
      set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  else()
      # This warning is not recognized by XCode/Clang for some reason.
      add_compile_options(-Wno-format-truncation)
  endif()
endif()

# Common CFLAGS (apply to all targets)
add_compile_options(
  -fno-inline-functions # Still relevant
  -fno-strict-aliasing # Still relevant
  -funsigned-char # Still relevant
  -fwrapv # Still relevant
  -include "${CMAKE_BINARY_DIR}/port/include/versioninfo.h" # Use absolute path to configured header
)

# Common warning flags (check if psp-gcc supports all)
add_compile_options(
  -Wall
  -Wno-address
  #-Wno-int-in-bool-context # Check if needed/supported
  -Wno-misleading-indentation
  -Wno-missing-braces
  -Wno-multichar
  -Wno-tautological-compare
  #-Wno-unused-but-set-variable # Check if needed/supported
  -Wno-unused-value
  -Wno-unused-variable
)

# C specific flags
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>)

# C++ specific flags
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>)

# --- Dependencies ---
set(LIBS)

if(BUILD_PSP)
  # --- PSP Dependencies ---

  # SDL2 (PSP Port) - Requires SDL-PSP installed within PSPSDK or findable via pkg-config
  # Option 1: Use pkg-config (Recommended if SDL-PSP provides .pc file)
  find_package(PkgConfig REQUIRED)
  find_package(SDL2 CONFIG REQUIRED)

	if(GCC_LEGACY)
	find_package(fmt REQUIRED)
	endif(GCC_LEGACY)

	link_libraries(SDL2::SDL2)
	link_libraries(${SDL2_LIBRARIES})
 

  # ZLib (PSP Port)
  pkg_check_modules(ZLIB REQUIRED zlib)
  if(ZLIB_FOUND)
    message(STATUS "Found ZLIB-PSP: ${ZLIB_LIBRARIES}")
    include_directories(SYSTEM ${ZLIB_INCLUDE_DIRS})
    list(APPEND LIBS ${ZLIB_LIBRARIES}) # Usually -lz
  else()
    message(STATUS "pkg-config failed for ZLIB-PSP, attempting manual find...")
    find_path(ZLIB_INCLUDE_DIR zlib.h PATHS ${PSPSDK}/psp/include NO_SYSTEM_ENVIRONMENT_PATH)
    find_library(ZLIB_LIBRARY NAMES z PATHS ${PSPSDK}/psp/lib NO_SYSTEM_ENVIRONMENT_PATH)
    if(NOT ZLIB_INCLUDE_DIR OR NOT ZLIB_LIBRARY)
        message(FATAL_ERROR "ZLIB for PSP not found. Ensure zlib is installed in your PSPSDK (${PSPSDK}).")
    endif()
    message(STATUS "Found ZLIB-PSP manually: ${ZLIB_LIBRARY}")
    include_directories(SYSTEM "${ZLIB_INCLUDE_DIR}")
    list(APPEND LIBS "${ZLIB_LIBRARY}")
  endif()

else()
  # --- Desktop/Switch Dependencies ---
  find_package(SDL2 REQUIRED)
  include_directories(SYSTEM "${SDL2_INCLUDE_DIR}")
  list(APPEND LIBS "${SDL2_LIBRARY}")

  find_package(ZLIB REQUIRED)
  if(ZLIB_FOUND)
      include_directories(SYSTEM "${ZLIB_INCLUDE_DIR}")
      list(APPEND LIBS "${ZLIB_LIBRARY}")
  endif()

  # GL (Not for PSP)
  if(NOT TARGET_OS STREQUAL "nswitch") # Switch has its own graphics setup usually
    if(WIN32)
        set(GL_LIBRARY opengl32)
    elseif(APPLE)
        set(GL_LIBRARY "-framework OpenGL")
    else() # Linux
        set(GL_LIBRARY GL)
    endif()
    list(APPEND LIBS "${GL_LIBRARY}")
  elseif(NINTENDO_SWITCH)
      # Add Switch specific graphics libs if needed (e.g., EGL, GL)
      # list(APPEND LIBS EGL glapi) # Example
  endif()


  # Platform-specific extra libs (Non-PSP)
  if(NOT EXTRA_LIBRARIES)
      if(WIN32)
          set(EXTRA_LIBRARIES dbghelp -static-libstdc++)
      elseif(NINTENDO_SWITCH)
          set(EXTRA_LIBRARIES stdc++ pthread) # Add Switch specific libs
      else() # Linux/macOS
          set(EXTRA_LIBRARIES stdc++ m dl pthread) # Added pthread for consistency
      endif()
  endif()
  list(APPEND LIBS ${EXTRA_LIBRARIES})

endif()


# --- Sources ---
# Use file(GLOB...) to find files. These usually return absolute paths.
file(GLOB_RECURSE SRC_PORT "${CMAKE_SOURCE_DIR}/port/*.c")
file(GLOB_RECURSE SRC_PORT_CXX "${CMAKE_SOURCE_DIR}/port/*.cpp")
file(GLOB_RECURSE SRC_GAME "${CMAKE_SOURCE_DIR}/src/game/*.c")
file(GLOB SRC_LIB_MP3 "${CMAKE_SOURCE_DIR}/src/lib/mp3/*.c")
file(GLOB SRC_LIB_NAUDIO "${CMAKE_SOURCE_DIR}/src/lib/naudio/*.c")
file(GLOB SRC_LIB_ULTRA_AUDIO "${CMAKE_SOURCE_DIR}/src/lib/ultra/audio/*.c")
file(GLOB SRC_LIB_GU "${CMAKE_SOURCE_DIR}/src/lib/ultra/gu/*.c")

# List manually specified source files RELATIVE to CMAKE_SOURCE_DIR
set(SRC_LIB_RELATIVE
  src/lib/ultra/io/vimodentsclan1.c
  src/lib/ultra/io/vimodepallan1.c
  src/lib/ultra/io/vimodempallan1.c
  src/lib/ultra/io/vitbl.c
  src/lib/ailist.c
  src/lib/anim.c
  src/lib/args.c
  src/lib/audiodma.c
  src/lib/audiomgr.c
  src/lib/base.c
  src/lib/collision.c
  src/lib/debughud.c
  src/lib/dma.c
  src/lib/fault.c # May need PSP specific fault handling
  src/lib/joy.c   # Needs porting/abstraction for pspctrl
  src/lib/lib_2f490_c.c
  src/lib/lib_2fc60.c
  src/lib/lib_3e3e0.c
  src/lib/lib_04f60nb.c
  src/lib/lib_17ce0.c
  src/lib/lib_034d0.c
  src/lib/lib_39c80.c
  src/lib/lib_15850.c
  src/lib/mema.c
  src/lib/memp.c
  src/lib/model.c
  src/lib/modelasm_c.c
  src/lib/mp3.c
  src/lib/mtx_c.c
  src/lib/mtx.c
  src/lib/music.c
  src/lib/path.c
  src/lib/profile.c # Profiling likely needs PSP specifics
  src/lib/rdp.c     # RDP emulation layer, check compatibility
  src/lib/rmon.c    # Remote monitor, likely disable/replace for PSP
  src/lib/rng_c.c
  src/lib/rzip_c.c
  src/lib/snd.c
  src/lib/speaker.c
  src/lib/varsinit.c
  src/lib/vi.c      # VI emulation layer, check compatibility
  src/textureconfig.c
)

# Convert the relative SRC_LIB_RELATIVE paths to absolute paths
set(SRC_LIB_ABSOLUTE "") # Initialize empty list
foreach(rel_path ${SRC_LIB_RELATIVE})
  list(APPEND SRC_LIB_ABSOLUTE "${CMAKE_SOURCE_DIR}/${rel_path}")
endforeach()


# Combine all the source file lists (which should now contain absolute paths)
set(SRC
  ${SRC_GAME}
  ${SRC_PORT}
  ${SRC_PORT_CXX}
  ${SRC_LIB_MP3}
  ${SRC_LIB_NAUDIO}
  ${SRC_LIB_ULTRA_AUDIO}
  ${SRC_LIB_GU}
  ${SRC_LIB_ABSOLUTE} # Use the list with absolute paths
)

# list(TRANSFORM SRC PREPEND "${CMAKE_SOURCE_DIR}/") # <<< REMOVED THIS LINE

# Exclude platform-specific source files (Keep this logic if needed)
if(NOT WIN32 OR BUILD_PSP)
    # Exclude windows specific files if any were added
endif()
if(NOT APPLE OR BUILD_PSP)
    # Exclude apple specific files if any were added
endif()
# Add PSP specific source files if necessary
# if(BUILD_PSP)
#   list(APPEND SRC "${CMAKE_SOURCE_DIR}/port/psp/psp_specific.c")
# endif()


# --- Asset Header Generation (Host Tool) ---
# This should still run on the host machine, so it's mostly unchanged.
# Ensure the tools path is correct. Using CMAKE_CURRENT_SOURCE_DIR might be safer.
generate_asset_headers("lang/" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mklang" "en" JSON_HEADERS)
generate_asset_headers("pads/" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mkpads" "" JSON_HEADERS)
generate_asset_headers("tiles/" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mktiles" "" JSON_HEADERS)
generate_asset_headers("animations.json" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mkanims" "" JSON_HEADERS)
generate_asset_headers("sequences.json" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mksequences" "" JSON_HEADERS)
generate_asset_headers("textures.json" "${CMAKE_CURRENT_SOURCE_DIR}/tools/assetmgr/mktextures" "" JSON_HEADERS)

add_custom_target(pd_headers ALL DEPENDS ${JSON_HEADERS})


# --- Game Target ---
set(BIN_NAME_ELF "pd${BIN_REGION}.${TARGET_ARCH}.elf") # Intermediate ELF name
set(BIN_NAME_EBOOT "EBOOT.PBP") # Final PSP executable
set(PD_TARGET_NAME "pd_elf") # Internal CMake target name for the ELF

# Use the combined SRC list directly
add_executable(${PD_TARGET_NAME} ${SRC})
add_dependencies(${PD_TARGET_NAME} pd_headers) # Depend on generated headers
target_link_libraries(${PD_TARGET_NAME} PRIVATE ${LIBS}) # Use PRIVATE for libraries
set_target_properties(${PD_TARGET_NAME} PROPERTIES OUTPUT_NAME "${BIN_NAME_ELF}")

list(APPEND LIBS pspgu pspgum pspge pspmath psppower pspdebug pspctrl pspdisplay pspaudiolib pspaudio pspgl )


# --- Post-Build Steps ---

if(BUILD_PSP)
create_pbp_file(
  TARGET pd_elf
  PREVIEW_PATH NULL
  TITLE pd
  BUILD_PRX
  )

elseif(NINTENDO_SWITCH)
  # --- Switch NRO Generation ---
  # Assuming elf2nro and nacptool are in PATH or found via toolchain
  find_program(ELF2NRO_EXECUTABLE NAMES aarch64-none-elf-elf2nro elf2nro REQUIRED) # Adjust name if needed
  find_program(NACPTOOL_EXECUTABLE NAMES nacptool REQUIRED)
  find_program(STRIP_EXECUTABLE NAMES aarch64-none-elf-strip strip REQUIRED)

  set(SWITCH_NACP_PATH "${CMAKE_BINARY_DIR}/${PD_TARGET_NAME}.nacp")
  set(SWITCH_STRIPPED_ELF_PATH "${CMAKE_BINARY_DIR}/${PD_TARGET_NAME}.stripped.elf")
  set(SWITCH_NRO_PATH "${CMAKE_BINARY_DIR}/pd${BIN_REGION}.${TARGET_ARCH}.nro") # Final NRO name
  set(SWITCH_ICON_PATH "${CMAKE_SOURCE_DIR}/dist/nswitch/icon.jpg") # Ensure this icon exists

  add_custom_command(TARGET ${PD_TARGET_NAME} POST_BUILD
      COMMAND ${NACPTOOL_EXECUTABLE} --create "Perfect Dark" "fgsfds and contributors" "${VERSION_ROMID}/${VERSION_BRANCH}/${VERSION_HASH}" ${SWITCH_NACP_PATH}
      COMMAND ${STRIP_EXECUTABLE} -o ${SWITCH_STRIPPED_ELF_PATH} $<TARGET_FILE:${PD_TARGET_NAME}>
      COMMAND ${ELF2NRO_EXECUTABLE} ${SWITCH_STRIPPED_ELF_PATH} ${SWITCH_NRO_PATH} --icon=${SWITCH_ICON_PATH} --nacp=${SWITCH_NACP_PATH}
      COMMENT "Generating Switch NRO"
      VERBATIM
  )
  # Define a target representing the final NRO for convenience if needed elsewhere
  add_custom_target(pd_nro ALL DEPENDS ${PD_TARGET_NAME}) # Depends implicitly via POST_BUILD

else()
  # --- Desktop Executable Configuration ---
  set_target_properties(${PD_TARGET_NAME} PROPERTIES OUTPUT_NAME "pd${BIN_REGION}.${TARGET_ARCH}") # Set final desktop name

  if(WIN32)
    set_target_properties(${PD_TARGET_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    # Add Windows icon resource if needed (ensure icon.rc exists and is compatible)
    # You would need to add the .rc file to the SRC list earlier if using this.
    # Example: file(GLOB RC_FILE "${CMAKE_SOURCE_DIR}/dist/windows/icon.rc")
    # list(APPEND SRC ${RC_FILE})
  endif()

  # Define a final target 'pd' aliasing the ELF target for consistency
  # Use add_custom_target + add_dependencies if you need build steps, ALIAS is just a name mapping
  add_library(pd ALIAS ${PD_TARGET_NAME})

endif()

# Installation (Optional, adapt paths as needed)
# install(TARGETS ${PD_TARGET_NAME} DESTINATION bin) # Install ELF/EXE
# if(BUILD_PSP)
#   install(FILES ${PSP_EBOOT_PATH} DESTINATION bin/psp) # Install EBOOT
# elseif(NINTENDO_SWITCH)
#    install(FILES ${SWITCH_NRO_PATH} DESTINATION bin/switch) # Install NRO
# endif()
# install(DIRECTORY ${ASSET_DIR}/ DESTINATION share/pd/assets) # Install assets