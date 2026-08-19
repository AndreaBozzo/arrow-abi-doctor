add_executable(abicase abicase.c)
target_link_libraries(abicase PRIVATE abi abi_fixture abi_flags)
