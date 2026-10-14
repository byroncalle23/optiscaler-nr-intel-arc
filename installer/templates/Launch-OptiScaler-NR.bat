@echo off
echo OptiScaler NR: iniciar como administrador y dejar la ventana abierta.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0nr-launcher\Start.ps1"
pause
