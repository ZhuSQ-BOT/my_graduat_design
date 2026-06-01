cmake_minimum_required(VERSION 3.16)
project(PsychServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql WebSockets)

# 显式列出所有源文件，避免 GLOB 问题
set(SOURCES
    src/main.cpp
    src/database/db_manager.cpp
    src/server/psych_server.cpp
    src/utils/logger.cpp
    src/server/handlers/admin_handler.cpp
    src/server/handlers/ai_chat_handler.cpp
    src/server/handlers/appointment_handler.cpp
    src/server/handlers/assessment_handler.cpp
    src/server/handlers/auth_handler.cpp
    src/server/handlers/counselor_handler.cpp
    src/server/handlers/favorites_handler.cpp
    src/server/handlers/forum_handler.cpp
    src/server/handlers/messaging_handler.cpp
    src/server/handlers/profile_handler.cpp
    src/server/handlers/report_handler.cpp
    src/server/handlers/resource_handler.cpp
    src/server/handlers/scale_crud_handler.cpp
)

set(HEADERS
    include/config.h
    include/message.h
    include/protocol.h
    include/database/db_manager.h
    include/server/psych_server.h
    src/utils/logger.h
)

add_executable(psych_server ${SOURCES} ${HEADERS})

target_include_directories(psych_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(psych_server PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::WebSockets
)
