cmake_minimum_required(VERSION 3.16)
project(oj_server CXX)

add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(oj_server
  src/main.cpp
  src/db/db_conn.cpp
  src/auth/password.cpp
  src/auth/session.cpp
  src/routes/auth_routes.cpp
  src/routes/problem_routes.cpp
)

target_include_directories(oj_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/util
  ${CMAKE_CURRENT_SOURCE_DIR}/src/db
  ${CMAKE_CURRENT_SOURCE_DIR}/src/auth
  ${CMAKE_CURRENT_SOURCE_DIR}/src/routes
)
target_compile_features(oj_server PRIVATE cxx_std_17)

# MySQL C API 头文件与链接
find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql)
find_library(MYSQL_LIBRARY mysqlclient)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
  message(FATAL_ERROR "MySQL C API not found (need libmysqlclient-dev)")
endif()

target_include_directories(oj_server PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(oj_server PRIVATE
  Threads::Threads
  OpenSSL::Crypto
  ${MYSQL_LIBRARY}
  ZLIB::ZLIB
)