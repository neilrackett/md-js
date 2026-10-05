# Create a library for the httpc files
add_library(httpc
    httpc.c
)

# Add the include directory (for httpc.h)
target_include_directories(httpc PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..   # add parent folder if lwipopts.h is there
)

# Link necessary libraries
target_link_libraries(httpc PRIVATE
    pico_stdlib    # Core Pico SDK library
    pico_lwip_http
)

# Conditionally link the CYW43 architecture library if supported
if (PICO_CYW43_SUPPORTED)
    # The firmware's own flavour (MDJS_CYW43_ARCH in ../CMakeLists.txt), so
    # httpc is built for the same async_context as the rest.
    target_link_libraries(httpc PRIVATE ${MDJS_CYW43_ARCH})
else()
    message(WARNING "CYW43 architecture not supported")
endif()
