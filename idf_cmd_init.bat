@echo off
:: ESP-IDF 环境初始化脚本
:: 将此目录添加到系统 PATH 后，可在任意位置运行 idf.py

:: 设置 IDF_PATH
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.2"

:: 运行 ESP-IDF 环境导出脚本
call "%IDF_PATH%\export.bat"

:: 保持窗口打开
cd /d "D:\nice_ai\sisi\client\sisi_esp"
echo.
echo ==========================================
echo ESP-IDF 环境已初始化！
echo 当前目录: %CD%
echo 现在可以直接运行 idf.py 命令了
echo 例如: idf.py menuconfig
echo         idf.py build
echo         idf.py -p COM4 flash
echo         idf.py -p COM4 monitor
echo ==========================================
echo.
cmd /k
