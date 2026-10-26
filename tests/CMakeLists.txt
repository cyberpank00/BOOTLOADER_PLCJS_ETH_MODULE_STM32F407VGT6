cmake_minimum_required(VERSION 3.22)
project(bootloader_host_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(test_modbus test_modbus.c)
target_compile_definitions(test_modbus PRIVATE NMBS_CLIENT_DISABLED NMBS_LITTLE_ENDIAN)
target_compile_options(test_modbus PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME modbus COMMAND test_modbus)
foreach(variant IN ITEMS boot boot_hw12 boot_hw21)
add_executable(test_${variant} test_boot.c
    ../Application/boot/boot_entry.c
    ../Application/flash/flash_if.c
    ../Application/flash/metadata.c
    ../Application/crc/crc32.c
    ../Application/validate/app_validate.c
    ../Application/installer/fw_installer.c
    ../Application/modbus/fw_update_proto.c)
target_include_directories(test_${variant} PRIVATE include
    ../Application/boot ../Application/flash ../Application/crc
    ../Application/validate ../Application/installer ../Application/modbus
    ../Application/third_party/nanomodbus ../Application/net_id
    ../Application/led ../Application/discovery ../LWIP/App)
target_compile_options(test_${variant} PRIVATE -Wall -Wextra -UNDEBUG -Wno-int-to-pointer-cast)
add_test(NAME ${variant} COMMAND test_${variant})
endforeach()
target_compile_definitions(test_boot_hw12 PRIVATE HW_REVISION_DEFAULT=0x010203u)
target_compile_definitions(test_boot_hw21 PRIVATE HW_REVISION_DEFAULT=0x020101u PRODUCT_ID_DEFAULT=0x504C0403u)
add_executable(test_tcp test_tcp.c ../Application/third_party/nanomodbus/nanomodbus.c)
target_include_directories(test_tcp PRIVATE include ../Application/modbus ../Application/flash
    ../Application/boot ../Application/third_party/nanomodbus)
target_compile_definitions(test_tcp PRIVATE NMBS_CLIENT_DISABLED NMBS_LITTLE_ENDIAN)
target_compile_options(test_tcp PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME tcp COMMAND test_tcp)
