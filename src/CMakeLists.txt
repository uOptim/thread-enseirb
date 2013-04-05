add_library (thread thread.c)

add_executable (contextes contextes.c)

add_executable (example example.c)
target_link_libraries (example thread)
