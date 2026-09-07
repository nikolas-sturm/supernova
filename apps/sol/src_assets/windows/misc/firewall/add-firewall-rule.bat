@echo off

rem Get sol root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set RULE_NAME=Sol
set PROGRAM_BIN="%ROOT_DIR%\sol.exe"

rem Add the rule
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=tcp program=%PROGRAM_BIN% enable=yes
netsh advfirewall firewall add rule name=%RULE_NAME% dir=in action=allow protocol=udp program=%PROGRAM_BIN% enable=yes
