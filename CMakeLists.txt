# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Some managed component Kconfig files depend on ESP_IDF_VERSION env var.
# Keep this in sync with the workspace IDF release to avoid parse failures.
if(NOT DEFINED ENV{ESP_IDF_VERSION} OR "$ENV{ESP_IDF_VERSION}" STREQUAL "")
    set(ENV{ESP_IDF_VERSION} "6.0.1")
endif()

# Some tool flows do not export ESP_ROM_ELF_DIR; set it from installed tools.
if(NOT DEFINED ENV{ESP_ROM_ELF_DIR} OR "$ENV{ESP_ROM_ELF_DIR}" STREQUAL "")
    file(GLOB _rom_elf_dirs "/home/dhaanesvar/.espressif/tools/esp-rom-elfs/*")
    list(SORT _rom_elf_dirs)
    list(REVERSE _rom_elf_dirs)
    foreach(_dir ${_rom_elf_dirs})
        if(EXISTS "${_dir}/esp32p4_rev0_rom.elf")
            set(ENV{ESP_ROM_ELF_DIR} "${_dir}")
            message(STATUS "Set ESP_ROM_ELF_DIR=${_dir}")
            break()
        endif()
    endforeach()
endif()

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(factory_demo VERSION 1.0.0)

# Suppress warnings only for esp-dl component
idf_component_get_property(esp_dl_lib espressif__esp-dl COMPONENT_LIB)
if(esp_dl_lib)
    target_compile_options(${esp_dl_lib} PRIVATE 
        -Wno-error=cpp
        -Wno-error=deprecated-declarations
        -Wno-unused-variable 
        -Wno-unused-but-set-variable
    )
    message(STATUS "Applied warning suppression to esp-dl component")
endif()
