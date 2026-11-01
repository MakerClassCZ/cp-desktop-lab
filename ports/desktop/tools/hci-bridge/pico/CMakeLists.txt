# The Pico 2 W HCI bridge (see ../README.md). PICO_SDK_PATH names the Pico SDK: the repository's
# own (ports/raspberrypi/sdk) with PICO_CYW43_DRIVER_PATH, PICO_LWIP_PATH and PICO_TINYUSB_PATH
# pointing at its submodules, or any Pico SDK 2.x.
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w CACHE STRING "")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(hci_bridge C CXX ASM)
pico_sdk_init()

add_executable(hci_bridge main.c)
target_compile_definitions(hci_bridge PRIVATE
    CYW43_ENABLE_BLUETOOTH=1
    CYW43_LWIP=0
    # The host need not raise DTR: a program that opens the port as a UART may not.
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
    USBD_PRODUCT="Pico HCI bridge"
)
target_link_libraries(hci_bridge pico_stdlib pico_cyw43_arch_poll cybt_shared_bus)
pico_enable_stdio_usb(hci_bridge 1)
pico_enable_stdio_uart(hci_bridge 0)
pico_add_extra_outputs(hci_bridge)
