add_executable(abicase abicase.c)
target_link_libraries(abicase PRIVATE abi abi_fixture abi_flags)

# The corpus generator. Separate from the `abicase` tool: that one inspects and
# verifies files a person already has, this one manufactures them in bulk from
# the model, and folding the two together would put the generator on the path
# of every triage command.
add_executable(abicase-gen gen_corpus.c)
target_link_libraries(abicase-gen PRIVATE abi abi_flags)
