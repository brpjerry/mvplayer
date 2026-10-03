cmake_minimum_required(VERSION 3.21)
set(MV_VERSION "0.1.1" CACHE STRING "Version recorded in the build")
project(mvplayer VERSION ${MV_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Sql)
qt_standard_project_setup(REQUIRES 6.5)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(CHROMAPRINT REQUIRED IMPORTED_TARGET libchromaprint)

# ---------------------------------------------------------------------------
# Core: library scanning, YouTube lookup, audio alignment, muxing. No GUI deps.
# ---------------------------------------------------------------------------
qt_add_library(mvcore STATIC
    src/core/Types.h
    src/core/Util.h src/core/Util.cpp
    src/core/Database.h src/core/Database.cpp
    src/core/TagReader.h src/core/TagReader.cpp
    src/core/Matcher.h src/core/Matcher.cpp
    src/core/YtDlp.h src/core/YtDlp.cpp
    src/core/AudioAlign.h src/core/AudioAlign.cpp
    src/core/Muxer.h src/core/Muxer.cpp
    src/core/LibraryScanner.h src/core/LibraryScanner.cpp
    src/core/ImportManager.h src/core/ImportManager.cpp
)
target_include_directories(mvcore PUBLIC src)
target_link_libraries(mvcore PUBLIC Qt6::Core Qt6::Sql PkgConfig::TAGLIB PkgConfig::CHROMAPRINT)

# Headless importer: same pipeline, no display needed.
qt_add_executable(mvplayer-import src/cli.cpp)
target_link_libraries(mvplayer-import PRIVATE mvcore)

option(MV_BUILD_GUI "Build the graphical player" ON)
if(NOT MV_BUILD_GUI)
    return()
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Qml Quick QuickControls2 Network OpenGL ShaderTools)
find_package(Qt6 QUIET OPTIONAL_COMPONENTS DBus)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

qt_add_executable(mvplayer
    src/main.cpp
    src/ui/MpvItem.h src/ui/MpvItem.cpp
    src/ui/AccentColor.h src/ui/AccentColor.cpp
    src/ui/SystemTheme.h src/ui/SystemTheme.cpp
    src/ui/VideoModel.h src/ui/VideoModel.cpp
    src/ui/AppController.h src/ui/AppController.cpp
    src/ui/FrameStats.h src/ui/FrameStats.cpp
    src/ui/IdleInhibitor.h src/ui/IdleInhibitor.cpp
    src/ui/IpcServer.h src/ui/IpcServer.cpp
)

set(MV_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/Icon.qml
    qml/Icons.qml
    qml/IconButton.qml
    qml/RoundedImage.qml
    qml/Sidebar.qml
    qml/SidebarItem.qml
    qml/TopBar.qml
    qml/SearchBox.qml
    qml/VideoGrid.qml
    qml/VideoCard.qml
    qml/PlayerView.qml
    qml/ControlBar.qml
    qml/SeekBar.qml
    qml/VolumeControl.qml
    qml/ImportPill.qml
    qml/ImportPanel.qml
    qml/SettingsPanel.qml
    qml/Welcome.qml
    qml/EqBars.qml
    qml/Spinner.qml
    qml/FlatButton.qml
    qml/FlatSwitch.qml
    qml/Tooltip.qml
    qml/KineticWheel.qml
)
foreach(f ${MV_QML_FILES})
    get_filename_component(fname ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${fname})
endforeach()
set_source_files_properties(qml/Theme.qml qml/Icons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(mvplayer
    URI MvPlayer
    VERSION 1.0
    QML_FILES ${MV_QML_FILES}
)

qt_add_shaders(mvplayer "mvplayer_shaders"
    PREFIX "/shaders"
    BASE shaders
    FILES shaders/rounded.frag
)

target_link_libraries(mvplayer PRIVATE
    mvcore
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::OpenGL
    PkgConfig::MPV
)
if(Qt6DBus_FOUND)
    target_link_libraries(mvplayer PRIVATE Qt6::DBus)
    target_compile_definitions(mvplayer PRIVATE MV_HAVE_DBUS)
endif()

include(GNUInstallDirs)
install(TARGETS mvplayer mvplayer-import RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/mvplayer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/mvplayer.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
