link_directories (${CMAKE_SOURCE_DIR}/src)

add_executable (01-main 01-main.c)
target_link_libraries (01-main thread)

add_executable (02-switch 02-switch.c)
target_link_libraries (02-switch thread)

add_executable (11-join 11-join.c)
target_link_libraries (11-join thread)

add_executable (12-join-main 12-join-main.c)
target_link_libraries (12-join-main thread)

add_executable (13-join-cascade 13-join-cascade.c)
target_link_libraries (13-join-cascade thread)

add_executable (21-create-many 21-create-many.c)
target_link_libraries (21-create-many thread)

add_executable (22-create-many-recursive 22-create-many-recursive.c)
target_link_libraries (22-create-many-recursive thread)

add_executable (31-switch-many 31-switch-many.c)
target_link_libraries (31-switch-many thread)

add_executable (32-switch-many-join 32-switch-many-join.c)
target_link_libraries (32-switch-many-join thread)

add_executable (51-fibonacci 51-fibonacci.c)
target_link_libraries (51-fibonacci thread)
