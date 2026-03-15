cmake_minimum_required(VERSION 3.16)
project(pd C CXX) # <<< MOVED THIS LINE UP
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20) # Check if psp-g++ supports C++20, might need 17 or 14
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "Target arch: ${TARGET_ARCH}")
message(CPU: ${CMAKE_SYSTEM_PROCESSOR})


if(PSP)
  set(TARGET_OS "psp")
  add_compile_definitions(GBI_FLOATS)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# --- macOS Deployment Target (Only if actually building for macOS) ---
if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum OS X deployment version")
  endif()
endif()

include(CPUTest)
include(FindPkgConfig)
include(AssetUtils)


# --- Build Type Default ---
if (NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# --- Options ---
set(ROMID "ntsc-final" CACHE STRING "Which ROM to build an executable for: ntsc-final, pal-final or jpn-final")


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


# --- Compiler Definitions ---
add_definitions(
  -DMATCHING=0
  -DPIRACYCHECKS=0
  -DROM_SIZE=32
  -DAVOID_UB=1
  -D_LANGUAGE_C=1
  -DPAL=${VERSION_IS_PAL}
  -DVERSION=${VERSION}
)

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
if(PSP)
  add_compile_options(
    -G0 # Often required for PSP startup code, handles global pointer. Toolchain might add this automatically.
    -Wall # Keep warnings enabled
    # Add other PSP specific flags as needed, e.g., -fno-builtin-printf for smaller code size if using pspDebugScreenPrintf
  )
  # Optimization: -O2 (optimize for speed) for PSP release builds
  # CMake sets optimization flags per build type, adjust if needed:
  if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
      message(STATUS "Using -O2 optimization for PSP ${CMAKE_BUILD_TYPE} build")
      set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g -O2 -ffast-math -flto -fipa-pta -fomit-frame-pointer")
      set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -O2 -ffast-math -flto -fipa-pta -fomit-frame-pointer")
      set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -g -O2 -ffast-math -flto -fipa-pta -fomit-frame-pointer")
      set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -g -O2 -ffast-math -flto -fipa-pta -fomit-frame-pointer")
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

if(PSP)
  # --- PSP Dependencies ---

  # SDL2 (PSP Port) - Requires SDL-PSP installed within PSPSDK or findable via pkg-config
  # Option 1: Use pkg-config (Recommended if SDL-PSP provides .pc file)
  find_package(PkgConfig REQUIRED)
 
	if(GCC_LEGACY)
	find_package(fmt REQUIRED)
	endif(GCC_LEGACY)

  # ZLib (PSP Port)
  pkg_check_modules(ZLIB REQUIRED zlib)
  if(ZLIB_FOUND)
    include_directories(SYSTEM ${ZLIB_INCLUDE_DIRS})
    list(APPEND LIBS ${ZLIB_LIBRARIES}) # Usually -lz
  endif()

else()
  # --- Desktop/Switch Dependencies ---
  find_package(SDL2 REQUIRED)
  include_directories(SYSTEM "${SDL2_INCLUDE_DIR}")
  list(APPEND LIBS "${SDL2_LIBRARY}")


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
      if(WIN32)
          set(EXTRA_LIBRARIES dbghelp -static-libstdc++)
      elseif(APPLE OR UNIX)
          set(EXTRA_LIBRARIES stdc++ pthread dl m pthread) 
      else()
          set(EXTRA_LIBRARIES stdc++) 
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
set(BIN_NAME_ELF "pd${BIN_REGION}.${TARGET_ARCH}elf") # Intermediate ELF name
set(PD_TARGET_NAME "pd_elf") # Internal CMake target name for the ELF

list(APPEND LIBS glut GLU GL pspge pspgu pspgum m pspmath pspfpu pspvfpu psppower pspdebug pspctrl pspdisplay pspaudiolib pspaudio )

# Use the combined SRC list directly
add_executable(pd ${SRC})
add_dependencies(pd pd_headers) # Depend on generated headers
target_link_libraries(pd PRIVATE ${LIBS}) # Use PRIVATE for libraries

# --- Post-Build Steps ---

if(PSP)
create_pbp_file(
  TARGET pd
  PREVIEW_PATH NULL
  ICON_PATH "${PROJECT_SOURCE_DIR}/dist/psp/ICON0.png"
  TITLE "Perfect Dark"
  BUILD_PRX
  )

elseif(${CMAKE_SYSTEM_NAME} EQUAL NintendoSwitch)
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

endif()

# Installation (Optional, adapt paths as needed)
message(${CMAKE_CURRENT_SOURCE_DIR})
message("Binary:" ${CMAKE_BINARY_DIR})
  install(FILES ${CMAKE_BINARY_DIR}/EBOOT.PBP DESTINATION PerfectDark)
  install(FILES ${CMAKE_BINARY_DIR}/pd.prx DESTINATION PerfectDark)

  install(DIRECTORY DESTINATION PerfectDark/data)
