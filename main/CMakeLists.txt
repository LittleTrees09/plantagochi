# main/CMakeLists.txt
# Registers the main component and lists all source files.
# If you add more .c files later, add them to SRCS.

idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
)