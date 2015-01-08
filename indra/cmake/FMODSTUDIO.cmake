# -*- cmake -*-

include(Linking)

if (FMODEX AND FMODSTUDIO)
    message( FATAL_ERROR "You can not enable two FMOD variants at the same time." )
endif (FMODEX AND FMODSTUDIO)

unset(FMOD_LIBRARY_RELEASE CACHE)
unset(FMOD_LIBRARY_DEBUG CACHE)
unset(FMOD_INCLUDE_DIR CACHE)

if(STANDALONE)
	if (NOT FMODSTUDIO_SDK_DIR AND WINDOWS)
	  GET_FILENAME_COMPONENT(REG_DIR [HKEY_CURRENT_USER\\Software\\FMOD\ Studio\ API\ Windows] ABSOLUTE)
	  set(FMODSTUDIO_SDK_DIR ${REG_DIR} CACHE PATH "Path to the FMOD Studio SDK." FORCE)
	endif (NOT FMODSTUDIO_SDK_DIR AND WINDOWS)
	if(NOT FMODSTUDIO_SDK_DIR)
	  message(FATAL_ERROR "FMODSTUDIO_SDK_DIR not set!")
	endif(NOT FMODSTUDIO_SDK_DIR)
endif(STANDALONE)

if(FMODSTUDIO_SDK_DIR)
  if(LINUX AND WORD_SIZE EQUAL 32)
    set(release_lib_paths ${release_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/x86/lib" )
    set(debug__lib_paths ${debug_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/x86/lib")
  elseif(LINUX)
    set(release__lib_paths ${release_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/x86_64/lib")
    set(debug_fmod_lib_paths ${debug_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/x86_64/lib")
  else(LINUX AND WORD_SIZE EQUAL 32)
    set(release_fmod_lib_paths ${release_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib")
    set(debug_fmod_lib_paths ${debug_fmod_lib_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib")
  endif(LINUX AND WORD_SIZE EQUAL 32)
  set(fmod_inc_paths ${fmod_inc_paths} "${FMODSTUDIO_SDK_DIR}/api/lowlevel/inc")

  if(LINUX AND WORD_SIZE EQUAL 32)
    set(release_lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib/x86" )
    set(debug__lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lig/x86")
  elseif(LINUX)
    set(release__lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib/x86_64")
    set(debug_fmod_lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib/x86_64")
  else(LINUX AND WORD_SIZE EQUAL 32)
    set(release_fmod_lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib")
    set(debug_fmod_lib_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/lib")
  endif(LINUX AND WORD_SIZE EQUAL 32)
  set(fmod_inc_paths "${FMODSTUDIO_SDK_DIR}/api/lowlevel/inc")

  if(WINDOWS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES_OLD ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .dll)
  endif(WINDOWS)
  if(WORD_SIZE EQUAL 64 AND WINDOWS)
    find_library(FMOD_LIBRARY_RELEASE fmod64 PATHS ${release_fmod_lib_paths})
    find_library(FMOD_LIBRARY_DEBUG fmodL64 PATHS ${debug_fmod_lib_paths}) 
  else(WORD_SIZE EQUAL 64 AND WINDOWS)#Check if CMAKE_FIND_LIBRARY_PREFIXES is set to 'lib' for darwin.
    find_library(FMOD_LIBRARY_RELEASE fmod PATHS ${release_fmod_lib_paths})
    find_library(FMOD_LIBRARY_DEBUG fmodL PATHS ${debug_fmod_lib_paths})
  endif(WORD_SIZE EQUAL 64 AND WINDOWS)
  if(WINDOWS)
	set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES_OLD})
	string(REPLACE ".dll" "_vc.lib" FMOD_LINK_LIBRARY_RELEASE ${FMOD_LIBRARY_RELEASE})
	string(REPLACE ".dll" "_vc.lib" FMOD_LINK_LIBRARY_DEBUG ${FMOD_LIBRARY_DEBUG})
  else(WINDOWS)
    set(FMOD_LINK_LIBRARY_RELEASE ${FMOD_LIBRARY_RELEASE})
    set(FMOD_LINK_LIBRARY_DEBUG ${FMOD_LIBRARY_DEBUG})
  endif(WINDOWS)
  find_path(FMOD_INCLUDE_DIR fmod.hpp ${fmod_inc_paths})
  if(NOT FMOD_LIBRARY_RELEASE OR NOT FMOD_INCLUDE_DIR)
    if(STANDALONE)
      message(FATAL_ERROR "Provided FMODSTUDIO_SDK_DIR path not found '{$FMODSTUDIO_SDK_DIR}'")
	else(STANDALONE)
	  message(STATUS "Provided FMODSTUDIO_SDK_DIR path not found '${FMODSTUDIO_SDK_DIR}'. Falling back to prebuilts")
	endif(STANDALONE)
  else(NOT FMOD_LIBRARY_RELEASE OR NOT FMOD_INCLUDE_DIR)
    message(STATUS "Using system-provided FMOD Studio Libraries")
  endif(NOT FMOD_LIBRARY_RELEASE OR NOT FMOD_INCLUDE_DIR)
endif (FMODSTUDIO_SDK_DIR)

if (NOT FMOD_LIBRARY_RELEASE OR NOT FMOD_INCLUDE_DIR)
  if(WINDOWS)
    set(lib_suffix .dll)
  elseif(DARWIN)
    set(lib_suffix .dynlib)
  else(WINDOWS)
    set(lib_suffix .so)
  endif(WINDOWS)
  if(WINDOWS)
    if(WORD_SIZE EQUAL 64) 
      set(FMOD_LIBRARY_RELEASE ${LIBS_PREBUILT_DIR}/lib/release/fmod64${lib_suffix})
      set(FMOD_LIBRARY_DEBUG ${LIBS_PREBUILT_DIR}/lib/debug/fmodL64${lib_suffix})
	else(WORD_SIZE EQUAL 64)
	  set(FMOD_LIBRARY_RELEASE ${LIBS_PREBUILT_DIR}/lib/release/fmod${lib_suffix})
      set(FMOD_LIBRARY_DEBUG ${LIBS_PREBUILT_DIR}/lib/debug/fmodL${lib_suffix})
	endif(WORD_SIZE EQUAL 64)
  else(WINDOWS)
    set(FMOD_LIBRARY_RELEASE ${LIBS_PREBUILT_DIR}/lib/release/libfmod${lib_suffix})
    set(FMOD_LIBRARY_DEBUG ${LIBS_PREBUILT_DIR}/lib/debug/libfmodL${lib_suffix})
  endif(WINDOWS)
  set(FMOD_LINK_LIBRARY_RELEASE ${FMOD_LIBRARY_RELEASE})
  set(FMOD_LINK_LIBRARY_DEBUG ${FMOD_LIBRARY_DEBUG})
  if(WINDOWS)
  	string(REPLACE ".dll" "_vc.lib" FMOD_LINK_LIBRARY_RELEASE ${FMOD_LIBRARY_RELEASE})
	string(REPLACE ".dll" "_vc.lib" FMOD_LINK_LIBRARY_DEBUG ${FMOD_LIBRARY_DEBUG})
  endif(WINDOWS)
  use_prebuilt_binary(fmodstudio)
  set(FMOD_INCLUDE_DIR
      ${LIBS_PREBUILT_DIR}/include/fmodstudio)
endif(NOT FMOD_LIBRARY_RELEASE OR NOT FMOD_INCLUDE_DIR)

if(FMOD_LIBRARY_RELEASE AND FMOD_INCLUDE_DIR)
  set(FMOD ON)
  if (NOT FMOD_LIBRARY_DEBUG) #Use release library in debug configuration if debug library is absent.
    set(FMOD_LIBRARY_DEBUG ${FMOD_LIBRARY_RELEASE})
  endif (NOT FMOD_LIBRARY_DEBUG)
else (FMOD_LIBRARY_RELEASE AND FMOD_INCLUDE_DIR)
  message(STATUS "No support for FMOD Studio audio (need to set FMODSTUDIO_SDK_DIR?)")
  set(FMOD OFF)
  set(FMODSTUDIO OFF)
endif (FMOD_LIBRARY_RELEASE AND FMOD_INCLUDE_DIR)

if (FMOD)
  message(STATUS "Building with FMOD Studio audio support")
endif (FMOD)
