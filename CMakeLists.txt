cmake_minimum_required(VERSION 3.21)
set(MV_VERSION "0.2.0" CACHE STRING "Version recorded in the build")
project(mvplayer VERSION ${MV_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Sql Network)
qt_standard_project_setup(REQUIRES 6.5)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
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
    src/core/AudioPrint.h src/core/AudioPrint.cpp
    src/core/Muxer.h src/core/Muxer.cpp
    src/core/TalkCheck.h src/core/TalkCheck.cpp
    src/core/Subtitles.h src/core/Subtitles.cpp
    src/core/ArtistChannels.h src/core/ArtistChannels.cpp
    src/core/LibraryScanner.h src/core/LibraryScanner.cpp
    src/core/ImportManager.h src/core/ImportManager.cpp
    src/core/ChatGptAuth.h src/core/ChatGptAuth.cpp
    src/core/ChatGptTransport.h src/core/ChatGptTransport.cpp
    src/core/Judge.h src/core/Judge.cpp src/core/JudgePrompts.cpp
)
target_include_directories(mvcore PUBLIC src)
target_link_libraries(mvcore PUBLIC Qt6::Core Qt6::Sql Qt6::Network OpenSSL::Crypto PkgConfig::TAGLIB PkgConfig::CHROMAPRINT)

# Windows: the icon and version details shown by Explorer.
if(WIN32)
    enable_language(RC)
    configure_file(packaging/windows/mvplayer.rc.in ${CMAKE_CURRENT_BINARY_DIR}/mvplayer.rc @ONLY)
    set(MV_WINDOWS_RC ${CMAKE_CURRENT_BINARY_DIR}/mvplayer.rc)
endif()

# Headless importer: same pipeline, no display needed.
qt_add_executable(mvplayer-import src/cli.cpp ${MV_WINDOWS_RC})
target_link_libraries(mvplayer-import PRIVATE mvcore)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(mvplayer-chatgpt-test tests/ChatGptTest.cpp)
    target_link_libraries(mvplayer-chatgpt-test PRIVATE mvcore Qt6::Test)
    add_test(NAME chatgpt COMMAND mvplayer-chatgpt-test)
    qt_add_executable(mvplayer-foldertree-test tests/FolderTreeTest.cpp src/ui/FolderTreeModel.h src/ui/FolderTreeModel.cpp)
    target_link_libraries(mvplayer-foldertree-test PRIVATE mvcore Qt6::Test)
    add_test(NAME foldertree COMMAND mvplayer-foldertree-test)
    qt_add_executable(mvplayer-albummodel-test tests/AlbumModelTest.cpp src/ui/AlbumModel.h src/ui/AlbumModel.cpp)
    target_link_libraries(mvplayer-albummodel-test PRIVATE mvcore Qt6::Test)
    add_test(NAME albummodel COMMAND mvplayer-albummodel-test)
    qt_add_executable(mvplayer-database-test tests/DatabaseTest.cpp)
    target_link_libraries(mvplayer-database-test PRIVATE mvcore Qt6::Test)
    add_test(NAME database COMMAND mvplayer-database-test)
    find_package(Qt6 COMPONENTS Gui)
    if(TARGET Qt6::Gui)
        qt_add_executable(mvplayer-covers-test tests/CoverCacheTest.cpp src/ui/CoverCache.h src/ui/CoverCache.cpp)
        target_link_libraries(mvplayer-covers-test PRIVATE mvcore Qt6::Gui Qt6::Test)
        add_test(NAME covers COMMAND mvplayer-covers-test)
    endif()
endif()

option(MV_BUILD_GUI "Build the graphical player" ON)
if(NOT MV_BUILD_GUI)
    return()
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui GuiPrivate Qml Quick QuickControls2 Network OpenGL ShaderTools)
# D-Bus carries the Linux desktop integration; Windows has its own calls.
if(NOT WIN32)
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS DBus)
endif()
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
# The system volume: the PulseAudio client library on Linux (PipeWire serves
# it too); Windows has Core Audio. Without either the setting does nothing.
if(NOT WIN32)
    pkg_check_modules(PULSE IMPORTED_TARGET libpulse)
endif()
if(WIN32)
    # One of mpv's dependencies in MSYS2 names the prefix by its Unix path
    # ("/ucrt64/include"), which is not a directory Windows knows; the
    # compiler searches the real one anyway.
    get_target_property(MV_MPV_INCLUDES PkgConfig::MPV INTERFACE_INCLUDE_DIRECTORIES)
    list(FILTER MV_MPV_INCLUDES EXCLUDE REGEX "^/")
    set_target_properties(PkgConfig::MPV PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${MV_MPV_INCLUDES}")
endif()

qt_add_executable(mvplayer
    src/main.cpp
    src/ui/MpvItem.h src/ui/MpvItem.cpp
    src/ui/AccentColor.h src/ui/AccentColor.cpp
    src/ui/SystemTheme.h src/ui/SystemTheme.cpp
    src/ui/VideoModel.h src/ui/VideoModel.cpp
    src/ui/AlbumModel.h src/ui/AlbumModel.cpp
    src/ui/CoverCache.h src/ui/CoverCache.cpp
    src/ui/CoverProvider.h src/ui/CoverProvider.cpp
    src/ui/FolderTreeModel.h src/ui/FolderTreeModel.cpp
    src/ui/AppController.h src/ui/AppController.cpp
    src/ui/FrameStats.h src/ui/FrameStats.cpp
    src/ui/IdleInhibitor.h src/ui/IdleInhibitor.cpp
    src/ui/MediaSession.h src/ui/MediaSession.cpp
    src/ui/IpcServer.h src/ui/IpcServer.cpp
    src/ui/YtDlpUpdater.h src/ui/YtDlpUpdater.cpp
    src/ui/WindowFrame.h src/ui/WindowFrame.cpp
    src/ui/PointerPacer.h src/ui/PointerPacer.cpp
    src/ui/SystemVolume.h src/ui/SystemVolume.cpp
    src/ui/SingleInstance.h src/ui/SingleInstance.cpp
    ${MV_WINDOWS_RC}
)
# No console window on Windows.
set_target_properties(mvplayer PROPERTIES WIN32_EXECUTABLE TRUE)

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
    qml/AlbumGrid.qml
    qml/AlbumCard.qml
    qml/ModePill.qml
    qml/NowPlayingPanel.qml
    qml/CoverOverlay.qml
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
    qml/FlatCheck.qml
    qml/FolderTree.qml
    qml/Tooltip.qml
    qml/KineticWheel.qml
    qml/WindowControls.qml
    qml/WindowDrag.qml
    qml/ReviewControls.qml
    qml/SourcePopup.qml
    qml/SourceInfo.qml
    qml/ReviewPanel.qml
    qml/ReplaceDialog.qml
    qml/ChannelDialog.qml
)
foreach(f ${MV_QML_FILES})
    get_filename_component(fname ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${fname})
endforeach()
set_source_files_properties(qml/Theme.qml qml/Icons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The tray icon: the app icon's shape in white, large enough for any panel (Qt hands a tray the one picture; of a multi-size .ico it took the 16 px one).
qt_add_resources(mvplayer "mvplayer_icons" PREFIX "/mvplayer" BASE data FILES data/mvplayer-tray.png)

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
    Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::OpenGL
    PkgConfig::MPV
)
if(WIN32)
    target_link_libraries(mvplayer PRIVATE dwmapi ole32 uuid)
endif()
if(PULSE_FOUND)
    target_link_libraries(mvplayer PRIVATE PkgConfig::PULSE)
    target_compile_definitions(mvplayer PRIVATE MV_HAVE_PULSE)
endif()
if(Qt6DBus_FOUND)
    target_sources(mvplayer PRIVATE src/ui/Mpris.h src/ui/Mpris.cpp)
    target_link_libraries(mvplayer PRIVATE Qt6::DBus)
    target_compile_definitions(mvplayer PRIVATE MV_HAVE_DBUS)
endif()

include(GNUInstallDirs)
install(TARGETS mvplayer mvplayer-import RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES data/mvplayer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/mvplayer.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
