add_library(abi STATIC
  src/arena.c
  src/bytes.c
  src/sha256.c
  src/fill.c
  src/case.c
  src/validate.c
  src/encode.c
  src/decode.c
)
target_include_directories(abi
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(abi PRIVATE abi_flags)

# Fixtures are shared by the test suite and the CLI, so the cross-architecture
# check compares one definition rather than two that happen to agree.
add_library(abi_fixture STATIC tests/fixture.c)
target_include_directories(abi_fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(abi_fixture PUBLIC abi PRIVATE abi_flags)

add_executable(abicase_tests tests/test_abicase.c)
target_link_libraries(abicase_tests PRIVATE abi abi_fixture abi_flags)
target_include_directories(abicase_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(abicase_tests PRIVATE
  ABI_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

add_test(NAME abicase COMMAND abicase_tests)
