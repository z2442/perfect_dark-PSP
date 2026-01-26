

# Processor 64 bit check
if(${CMAKE_SIZEOF_VOID_P} GREATER "4")  #Do a 64 bit check
  message("Platform is 64 bit")
  add_definitions(-DPLATFORM_64BIT=1)
  else()
  message("Platform is 32 bit")
endif()

##endian Test
include (TestBigEndian)
	TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
	if(IS_BIG_ENDIAN)
		message(STATUS "Big Endian Detected")
		option(TARGET_IS_BIG_ENDIAN "Big Endian Processor" ON)
	else()
		message(STATUS "Little Endian Detected")
		option(TARGET_IS_BIG_ENDIAN "Litle Endian Processor" OFF)
	endif()
