cmake_minimum_required(VERSION 3.18.1)

project(usb_audio_output)

# Build the USB audio output library
add_library(usb_audio_output SHARED
    usb_audio_output.c
)

# Include libusb headers
target_include_directories(usb_audio_output PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libusb/include
)

# Link against prebuilt libusb
add_library(libusb SHARED IMPORTED)
set_target_properties(libusb PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/../libusbLib/${ANDROID_ABI}/libusb1.0.so
)

# Link libraries
target_link_libraries(usb_audio_output
    libusb
    log
)

# Set compile options
target_compile_options(usb_audio_output PRIVATE
    -Wall
    -Wextra
    -O2
    -DLOG_TAG=\"UsbAudioOutput\"
)
