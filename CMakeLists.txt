# Call of Duty 2: Big Red One (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(CoD2BRO default.xbe game.toml ${GAME_PARTITIONS} game)

# the menu pointer, embedded (src/common/software_cursor.cpp; cursor.png from MOHRS-XBERecomp), and the Controller
# screen's Modern picture (src/game/pad_picture.cpp; modern_controller.png by jojo8479)
if(WIN32)
    set(cursor_resource ${CMAKE_CURRENT_BINARY_DIR}/cursor.rc)
    configure_file(src/cursor.rc.in ${cursor_resource} @ONLY)
    set_source_files_properties(${cursor_resource} PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cursor.png;${CMAKE_CURRENT_SOURCE_DIR}/modern_controller.png")
    target_sources(CoD2BRO PRIVATE ${cursor_resource})
    # the look-alike fonts sharp text falls back on, embedded (src/game/sharp_text.cpp; fonts/, SIL OFL)
    set(fonts_resource ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
    configure_file(src/fonts.rc.in ${fonts_resource} @ONLY)
    file(GLOB font_files CONFIGURE_DEPENDS fonts/*.ttf)
    set_source_files_properties(${fonts_resource} PROPERTIES OBJECT_DEPENDS "${font_files}")
    target_sources(CoD2BRO PRIVATE ${fonts_resource})
    # the exe's icon: the game's own title image, taken from the player's default.xbe as it is built
    # (tools/extract_xbe_icon.py, after MOHRS-XBERecomp), never kept in the repository. No Python or no default.xbe:
    # no icon.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
        set(icon ${CMAKE_CURRENT_BINARY_DIR}/icons/CoD2BRO.ico)
        set(icon_resource ${CMAKE_CURRENT_BINARY_DIR}/icons/CoD2BRO.rc)
        add_custom_command(
            OUTPUT ${icon} ${icon_resource}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_xbe_icon.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe "$$XTIMAGE" ${icon}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_xbe_icon.py ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe
            COMMENT "Taking the game's icon from default.xbe"
            VERBATIM)
        set_source_files_properties(${icon_resource} PROPERTIES OBJECT_DEPENDS ${icon})
        target_sources(CoD2BRO PRIVATE ${icon_resource})
    else()
        message(STATUS "No Python or no assets/default.xbe: the exe is built without the game's icon")
    endif()
endif()
