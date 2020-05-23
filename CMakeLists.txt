cmake_minimum_required(VERSION 3.16)
project(words_mp)

set(CMAKE_CXX_STANDARD 14)

find_package(LibArchive REQUIRED)
find_package( ICU REQUIRED COMPONENTS uc i18n )

set (Boost_USE_STATIC_LIBS OFF)
set (Boost_USE_MULTITHREAD ON)

find_package(Boost REQUIRED COMPONENTS system filesystem locale)
add_executable(words_mp main.cpp)
find_package(Threads REQUIRED)
target_link_libraries(words_mp Threads::Threads)
target_link_libraries(words_mp Boost::system Boost::filesystem Boost::locale)
target_link_libraries(words_mp archive)