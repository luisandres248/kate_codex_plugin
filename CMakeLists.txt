cmake_minimum_required(VERSION 3.16)

project(katecodexpanel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(KDE_INSTALL_PLUGINDIR "${CMAKE_INSTALL_LIBDIR}/qt6/plugins/kf6" CACHE PATH "Qt6 KDE plugin install dir" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(KF6 REQUIRED COMPONENTS CoreAddons I18n TextEditor XmlGui Config)

kcoreaddons_add_plugin(katecodexpanel INSTALL_NAMESPACE "ktexteditor")
set_target_properties(katecodexpanel PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

target_sources(katecodexpanel PRIVATE
    plugin.cpp
    plugin.h
    plugin.json
    resources.qrc
)

target_link_libraries(katecodexpanel PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::TextEditor
    KF6::XmlGui
    KF6::ConfigCore
)

target_compile_features(katecodexpanel PRIVATE cxx_std_17)
