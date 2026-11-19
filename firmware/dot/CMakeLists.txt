# Sistema de build: https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# Estas líneas tienen que ir en este orden exacto
cmake_minimum_required(VERSION 3.5)

# Componentes compartidos por el hub y el dot. Cada carpeta de la lista contiene varios componentes
set(COMPONENTS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(EXTRA_COMPONENT_DIRS
    "${COMPONENTS_ROOT}/sensors"      # drivers I2C: leen el chip
    "${COMPONENTS_ROOT}/algorithms"   # interpretan las lecturas
    "${COMPONENTS_ROOT}/net")         # WiFi y subida a la base de datos

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hub-dot-dot)
