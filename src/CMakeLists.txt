add_library (thread thread.c)

add_executable (contextes contextes.c)

add_executable (example example.c)
target_link_libraries (example thread)

add_executable (example-pthread example-pthread.c)
target_link_libraries (example-pthread pthread)

add_executable (exemple_preemption exemple_preemption.c)
target_link_libraries (exemple_preemption pthread)
