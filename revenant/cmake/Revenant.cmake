# Helpers shared by Revenant slices: gtest wiring against the build
# tree's llvm_gtest and corpus-aware test defines.

# revenant_add_unittest(<suite> <test-target> sources...)
# Builds a gtest binary linked against the prebuilt llvm_gtest /
# llvm_gtest_main from build-revenant, and registers it under <suite>.
function(revenant_add_unittest suite name)
  add_llvm_executable(${name} NO_INSTALL_RPATH ${ARGN})
  add_dependencies(${suite} ${name})
  set_target_properties(${name} PROPERTIES FOLDER "Revenant tests")

  # gtest sources live in the LLVM checkout, next to LLVM_MAIN_INCLUDE_DIR
  # (llvm/include -> repo root is ../..).
  get_filename_component(_llvm_src_root "${LLVM_MAIN_INCLUDE_DIR}/../.." ABSOLUTE)
  target_include_directories(${name} PRIVATE
    "${_llvm_src_root}/third-party/unittest/googletest/include"
    "${_llvm_src_root}/third-party/unittest/googlemock/include"
    "${_llvm_src_root}/third-party/unittest/googletest"
    "${_llvm_src_root}/third-party/unittest/googlemock")

  # llvm_gtest targets are not exported; locate the libraries on disk.
  find_library(REVENANT_GTEST_LIB NAMES llvm_gtest PATHS "${LLVM_LIBRARY_DIRS}"
               NO_DEFAULT_PATH REQUIRED)
  find_library(REVENANT_GTEST_MAIN_LIB NAMES llvm_gtest_main
               PATHS "${LLVM_LIBRARY_DIRS}" NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(${name} PRIVATE
    ${REVENANT_GTEST_MAIN_LIB} ${REVENANT_GTEST_LIB})

  target_compile_definitions(${name} PRIVATE
    LLVM_BUILDING_REVENANT_TEST=1
    GTEST_HAS_TR1_TUPLE=0)

  if(WIN32 AND NOT CYGWIN)
    target_compile_definitions(${name} PRIVATE
      GTEST_HAS_SEH=0 GTEST_USE_OWN_TR1_TUPLE=0)
  endif()
endfunction()
