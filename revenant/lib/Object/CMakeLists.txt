add_llvm_library(RevenantObject
  PeFile.cpp
  FileMap.cpp

  LINK_COMPONENTS
  Object
  BinaryFormat
  Support

  LINK_LIBS
  )
