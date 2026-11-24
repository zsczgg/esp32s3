@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ============================================
echo  云打印盒 - 电脑端打印代理
echo ============================================
echo.
python print_agent.py
echo.
echo 代理已退出。
pause
