# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# ====== 版本号唯一真相源：只改这里！======
set(PROJECT_VERSION "20260925.0079")
# ★ 强制同步版本号到构建实际使用的 sdkconfig（2026-09-17 根修）：
#   分板构建用 -DSDKCONFIG=sdkconfig.{board} 指定配置并被 CMakeCache 缓存，
#   原写法盖写死的 ${CMAKE_SOURCE_DIR}/sdkconfig——该文件根本不参与编译，
#   导致版本号只升文件不升固件（OTA 端永远读到旧版本）。改为解析 SDKCONFIG：
#   未指定时回退默认 sdkconfig，相对路径锚定到工程根目录。
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig")
endif()
if(NOT IS_ABSOLUTE "${SDKCONFIG}")
    set(SDKCONFIG "${CMAKE_SOURCE_DIR}/${SDKCONFIG}")
endif()
execute_process(COMMAND sed -i "s/CONFIG_APP_PROJECT_VER=\"[^\"]*\"/CONFIG_APP_PROJECT_VER=\"${PROJECT_VERSION}\"/" "${SDKCONFIG}")
# ===========================================

project(NiceAI)

# ====== 合并固件生成（2026-09-12 根因修复）======
# 使用 IDF 5.5 内置 target：idf.py merge-bin（依赖 gen_project_binary+bootloader，
# 用 flash_args 自动合并），编译后必须执行 idf.py merge-bin 生成 merged-binary.bin。
# 背景：此前 merged-binary.bin 为 09:54 手动生成的一次性文件，多次发布误发陈旧内容
# （内嵌 20260912.0001），用户烧录后又被 OTA 覆盖。发布验证铁律：strings 提取产物
# 内嵌版本+编译时间戳核对后才允许发布。
# =================================================
