function(usbfn_options target)
    target_compile_options(${target} PRIVATE -std=c11 -Wall -Werror -Wno-unused-parameter)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
endfunction()

grimace_add_library(usbserial usbserial.c
    OUTPUT usbserial.dll
    EXPORTS USC_Init USC_Deinit USC_Open USC_Close USC_Read USC_Write USC_Seek USC_IOControl USC_PowerUp USC_PowerDown)
target_link_libraries(usbserial PRIVATE grimace::ceddk)
usbfn_options(usbserial)

grimace_add_executable(usbctl usbctl.c activate.c)
usbfn_options(usbctl)
