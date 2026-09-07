@echo off

set RULE_NAME=Sol

rem Delete the rule
netsh advfirewall firewall delete rule name=%RULE_NAME%
