set(sources
                 src/Allocator.cpp
                 src/APFloat.cpp
                 src/APInt.cpp
                 src/CommandLine.cpp
                 src/SmallPtrSet.cpp
                 src/SmallVector.cpp
                 src/StringMap.cpp
                 src/StringRef.cpp
                 src/Twine.cpp
)

add_library(gllvm STATIC ${sources})
target_include_directories(gllvm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(gllvm PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/include)
include(llvm-extras)

add_library(gllvm_obj OBJECT ${sources})
target_include_directories(gllvm_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(gllvm_obj PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/include)
set_target_properties (gllvm_obj PROPERTIES 
  INTERFACE_POSITION_INDEPENDENT_CODE On 
  POSITION_INDEPENDENT_CODE On
)

