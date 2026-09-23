add_executable(abicase abicase.c)
target_link_libraries(abicase PRIVATE abi abi_fixture abi_flags)

# The corpus generator. Separate from the `abicase` tool: that one inspects and
# verifies files a person already has, this one manufactures them in bulk from
# the model, and folding the two together would put the generator on the path
# of every triage command.
add_executable(abicase-gen gen_corpus.c)
target_link_libraries(abicase-gen PRIVATE abi abi_flags)

# Corpus B1, the enumerated invalid-producer cases. Hand-written and committed;
# tools/check_b1.py runs this and requires the committed bytes to match, so a
# case cannot drift from the definition beside its clause.
add_executable(abicase-gen-b1 gen_b1.c)
target_link_libraries(abicase-gen-b1 PRIVATE abi abi_fixture abi_flags)
