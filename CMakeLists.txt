# The PicoCalc FTP server: the SD card, over Wi-Fi, to any FTP client.
#
#   git submodule update --init --recursive
#   cmake -S . -B build -DPICO_SDK_PATH=/path/to/pico-sdk
#   cmake --build build            ->  build/ftp.uf2
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(ftp C CXX ASM)
pico_sdk_init()

set(MODULES ${CMAKE_CURRENT_LIST_DIR}/modules)
set(TS ${MODULES}/picocalc-text-starter)
set(DRV ${TS}/drivers)

# picocalc-extras: the card, and Wi-Fi with listening (control, data, and
# one to turn away a second client)
include(${MODULES}/picocalc-extras/fatfs/fatfs.cmake)
picocalc_fatfs(${TS})
set(PICOCALC_NET_CONNS 3)
include(${MODULES}/picocalc-extras/net/net.cmake)
picocalc_net(${MODULES}/picocalc-shell)

add_executable(ftp
    src/main.c src/ftpd.c
    ${DRV}/picocalc.c ${DRV}/display.c ${DRV}/clib.c ${DRV}/audio.c ${DRV}/onboard_led.c
    ${DRV}/font-5x10.c ${DRV}/font-8x10.c
    ${DRV}/keyboard.c ${DRV}/lcd.c ${DRV}/southbridge.c
)
pico_generate_pio_header(ftp ${DRV}/audio.pio)
target_include_directories(ftp PRIVATE src ${TS} ${DRV} ${MODULES}/picocalc-shell/include)
pico_set_program_name(ftp "ftp")
pico_set_program_version(ftp "0.1")
pico_enable_stdio_uart(ftp 0)
pico_enable_stdio_usb(ftp 0)
target_compile_definitions(ftp PRIVATE CYW43_HOST_NAME="picocalc-ftp")
target_link_libraries(ftp picocalc_net picocalc_fs
    pico_stdlib pico_rand pico_status_led hardware_gpio hardware_i2c hardware_spi hardware_pio
    hardware_clocks hardware_powman)
pico_add_extra_outputs(ftp)
