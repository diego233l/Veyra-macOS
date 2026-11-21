cmake_minimum_required(VERSION 3.21)
project(VeyraMac VERSION 2.1.9 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Multimedia ShaderTools Svg)
qt_standard_project_setup(REQUIRES 6.8)

# Bridge stubs generated from Veyra's own QmlPlayerBridge.h (kept in the tree
# so the build does not need the Windows sources).
set(VEYRA_SOURCES
  src/main.cpp
  src/Log.cpp
  src/UiLanguage.cpp
  src/StartupMusic.cpp
  src/StartupMusic.h
  src/MacBridge.cpp
  src/MacBridge.h
  src/generated/MacBridgeBase.cpp
  src/generated/MacBridgeBase.h
)
# Every file under src/parts/ belongs to an area of MacBridge; new ones are picked up automatically.
file(GLOB VEYRA_PARTS CONFIGURE_DEPENDS src/parts/*.cpp src/parts/*.h)
list(APPEND VEYRA_SOURCES ${VEYRA_PARTS})

qt_add_executable(Veyra MACOSX_BUNDLE ${VEYRA_SOURCES})
target_include_directories(Veyra PRIVATE src)
target_compile_definitions(Veyra PRIVATE VEYRA_VERSION="${PROJECT_VERSION}")
file(GLOB VEYRA_MAC_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} mac/*)
qt_add_resources(Veyra "veyra_mac_qml" PREFIX "/mac" BASE "${CMAKE_CURRENT_SOURCE_DIR}/mac" FILES ${VEYRA_MAC_QML})
qt_add_resources(Veyra "veyra_i18n" PREFIX "/i18n" BASE "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
  FILES i18n/catalog.json i18n/mac-catalog.json)
target_link_libraries(Veyra PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick
  Qt6::QuickControls2 Qt6::Multimedia Qt6::Svg)

file(GLOB VEYRA_SHADERS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} shaders/*.frag shaders/*.vert)
# Enhancement shaders (Qt RHI: Metal / OpenGL on macOS).
qt_add_shaders(Veyra "veyra_shaders" PREFIX "/shaders" BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  FILES ${VEYRA_SHADERS})

set_target_properties(Veyra PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER com.likely7.veyra.mac
  MACOSX_BUNDLE_BUNDLE_NAME Veyra
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in)

# QML front end and media next to the executable (Linux test build) or in
# Contents/Resources (macOS bundle).
if(APPLE)
  set(VEYRA_RES_DIR "$<TARGET_BUNDLE_CONTENT_DIR:Veyra>/Resources")
  target_sources(Veyra PRIVATE macos/veyra.icns)
  set_source_files_properties(macos/veyra.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(Veyra PROPERTIES MACOSX_BUNDLE_ICON_FILE veyra.icns)
else()
  set(VEYRA_RES_DIR "$<TARGET_FILE_DIR:Veyra>")
endif()
add_custom_command(TARGET Veyra POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/qml ${VEYRA_RES_DIR}/qml
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/media ${VEYRA_RES_DIR}/media
  COMMENT "Copying QML and media")
