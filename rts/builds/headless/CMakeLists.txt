# Place executables and shared libs under "build-dir/",
# instead of under "build-dir/rts/"
# This way, we have the build-dir structure more like the install-dir one,
# which makes testing spring in the builddir easier, eg. like this:
# cd build-dir
# SPRING_DATADIR=$(pwd) ./spring
SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

ADD_DEFINITIONS(-DHEADLESS)
ADD_DEFINITIONS(-DNO_SOUND)
ADD_DEFINITIONS(-DBITMAP_NO_OPENGL)
REMOVE_DEFINITIONS(-DAVI_CAPTURING)

IF    (MSVC OR MINGW OR APPLE)
	# Windows:
	# We still need these header files,
	# even if we are not going to link with gl, glu and SDL.
	# We have them available anyway (mingwlibs).
	# OS X:
	# Cocoa requires the SDL libary, whenever the SDL headers are used,
	# due to some #define magic, which is practically impossible to workaround.
	FIND_PACKAGE(OpenGL REQUIRED)
	FIND_PACKAGE(SDL REQUIRED)
	INCLUDE_DIRECTORIES(${SDL_INCLUDE_DIR})
	IF	  (MSVC)
		# The glext.h is Khronos supplied (See "Table 5. GL on Microsoft Windows" http://www.khronos.org/registry/implementers_guide.html),
		# add include to then use this 'direct copy' (See comment below) of the glext.h header.
		INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include/GLextKhronos)
	ENDIF (MSVC)
ELSE  (MSVC OR MINGW OR APPLE)
	# Use a direct copy of the GL and SDL headers,
	# as these may not be available on headless systems.
	INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include)
	INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include/SDL)
ENDIF (MSVC OR MINGW OR APPLE)


# headlessstubs are our stubs that replace libGL, libGLU, libGLEW, libSDL (yes really!)
LIST(APPEND engineHeadlessLibraries headlessStubs)
LIST(APPEND engineHeadlessLibraries no-sound)
LIST(APPEND engineHeadlessLibraries engineSystemNet)
LIST(APPEND engineHeadlessLibraries ${engineCommonLibraries})
LIST(APPEND engineHeadlessLibraries engineaGui)


INCLUDE_DIRECTORIES(${ENGINE_SRC_ROOT_DIR}/lib/assimp/include)

### Build the executable
ADD_EXECUTABLE(engine-headless ${engineSources} ${ENGINE_ICON})
TARGET_LINK_LIBRARIES(engine-headless ${engineHeadlessLibraries})

### CXX_FLAGS
if    (MSVC)
    MsvcTargetAddFlags_VmgMp(engine-legacy)
endif (MSVC)

IF    (MSVC)
    # MSVC Undefined symbol error for 'main'
    #
    # engine-headless main method is located in rts/System/Main.cpp.
    # Notice that 'main' is defined - But SDL_Main.h included earlier macro defines main to 'SDL_main'.
    #
    # http://wiki.libsdl.org/FAQWindows#I_get_.22Undefined_reference_to_.27SDL_main.27.22_...
    # http://wiki.libsdl.org/FAQWindows#I_get_.22Undefined_reference_to_.27WinMain.4016.27.22
    #   The SDL FAQ indeed states linking with SDL_Main is required - The replacement main performs extra initialization.
    #
    # headlessStubs contains replacement for SDL but not SDLMain
    # In FindSDL.cmake SDL headers one of which is SDLMain added to the SDL_INCLUDE_DIR variable
    # In FindSDL.cmake SDLMain added to the SDL_LIBRARY variable
    #   engine-legacy: SET(engineIncludes ${SDL_INCLUDE_DIR})
    #                  SET(engineLibraries ${SDL_LIBRARY})
    #   engine-headless: INCLUDE_DIRECTORIES(${SDL_INCLUDE_DIR})
    #                    LIST(APPEND engineHeadlessLibraries headlessStubs)
    # With headlessStubs the linked SDL is stub but the linked SDLMain is real
    #
    # Headless summary wrt SDL:
    #   Real SDL includs are used
    #   Stub SDL libraries are used (Of which, SDLMain is unimplemented)
    #
    # Why the heck does the build work on other platforms but fails on MSVC (And APPLE)?
    #   SDL_Main.h triggers main replacement only if
    #     #if defined(__WIN32__) || \
    #         (defined(__MWERKS__) && !defined(__BEOS__)) || \
    #         defined(__MACOS__) || defined(__MACOSX__) || \
    #         defined(__SYMBIAN32__) || defined(QWS)
    #   Other platforms do not get their main replaced
    #
    # The solution is to avoid including SDL_Main.h (Or most any other SDL header, SDl.h for example includes SDL_Main.h itself)
    #   Avoiding inclusion is only neccessary
    #     Location:  Where 'main' is defined rts/System/Main.cpp
    #     Condition: When stub SDL libraries are used
    # The correct snippet for use in Main.cpp is
    #   #if !defined(HEADLESS)
    #   #include <SDL_main.h>
    #   #endif

    # The older 'solution' - Causes building with /SUBSYSTEM:WINDOWS
    #set_target_properties(engine-headless PROPERTIES WIN32_EXECUTABLE "TRUE")
ENDIF (MSVC)

IF    (MINGW)
	# To enable console output/force a console window to open
	SET_TARGET_PROPERTIES(engine-headless PROPERTIES LINK_FLAGS "-Wl,-subsystem,console")
ENDIF (MINGW)


### Install the executable
INSTALL(TARGETS engine-headless DESTINATION ${BINDIR})

# Only build & install spring-headless executable & dependencies
# use cases:
# * make spring-headless
# * make install-spring-headless
CreateEngineBuildAndInstallTarget(headless)

