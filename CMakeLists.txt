cmake_minimum_required(VERSION 3.20...4.3)
project(UTF8Fixer LANGUAGES CXX C)

# ==============================================================================
# 1. 全局配置
# ==============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# AE SDK 路径配置
set(AE_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/AE_SDK" CACHE PATH "Path to Adobe After Effects SDK")

# ==============================================================================
# 2. 依赖管理 - 自动集成 MinHook
# ==============================================================================

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY "https://github.com/TsudaKageyu/minhook.git"
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# ==============================================================================
# 3. 目标构建与依赖关联
# ==============================================================================

add_library(${PROJECT_NAME} MODULE UTF8Fixer.cpp)

# 包含 SDK 与头文件目录
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${AE_SDK_PATH}/Headers 
    ${AE_SDK_PATH}/Headers/SP 
    ${AE_SDK_PATH}/Util 
    ${AE_SDK_PATH}/Resources
)

# 链接核心依赖库
target_link_libraries(${PROJECT_NAME} PRIVATE 
    minhook 
    user32.lib 
    kernel32.lib
)

# ==============================================================================
# 4. 性能优化与 PiPL 资源编译
# ==============================================================================

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS MSWindows WIN32)

    # MSVC 优化设置
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE
            /O1                         # 优化体积
            /GF                         # 启用只读字符串池化
            /Gy                         # 启用函数级链接
            /Oi                         # 启用内部函数
            $<$<CONFIG:Release>:/GL>    # Release 模式下启用全程序优化
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            /OPT:REF                    # 移除未引用代码与数据
            /OPT:ICF                    # 合并相同的 COMDAT 块
            $<$<CONFIG:Release>:/LTCG>  # Release 模式下启用链接时间代码生成
        )
    endif()

    # 重命名输出文件为 AE 规范的插件名称与后缀 (.aex)
    set_target_properties(${PROJECT_NAME} PROPERTIES 
        OUTPUT_NAME "!00_UTF8_Fixer" 
        SUFFIX ".aex"
    )
    
    # 资源编译中间路径定义
    set(AFX_REZ   "${AE_SDK_PATH}/Resources/PiPLtool.exe")
    set(PIPL_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/UTF8FixerPiPL.r")
    set(RR_FILE   "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rr")
    set(RRC_FILE  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rrc")
    set(RC_FILE   "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rc")
    
    # 步骤 1: 预处理 .r 资源文件
    add_custom_command(
        OUTPUT ${RR_FILE}
        COMMAND cl /utf-8 
                   /I "${AE_SDK_PATH}/Headers" 
                   /I "${AE_SDK_PATH}/Headers/SP" 
                   /I "${AE_SDK_PATH}/Util" 
                   /I "${AE_SDK_PATH}/Resources" 
                   /D "MSWindows" 
                   /EP "${PIPL_SRC}" > "${RR_FILE}"
        DEPENDS "${PIPL_SRC}"
        VERBATIM
    )
    
    # 步骤 2: 使用 PiPLtool 编译中间资源
    add_custom_command(
        OUTPUT ${RRC_FILE}
        COMMAND "${AFX_REZ}" "${RR_FILE}" "${RRC_FILE}"
        DEPENDS ${RR_FILE}
    )
    
    # 步骤 3: 再次调用预处理器生成标准 .rc 描述文件
    add_custom_command(
        OUTPUT ${RC_FILE}
        COMMAND cl /utf-8 /D "MSWindows" /EP "${RRC_FILE}" > "${RC_FILE}"
        DEPENDS ${RRC_FILE}
        VERBATIM
    )

    # 将生成的资源编译结果链入目标
    target_sources(${PROJECT_NAME} PRIVATE ${RC_FILE})
endif()