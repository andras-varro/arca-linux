@echo off
REM Servlet Compilation Script for Arca LED Control (Windows)

echo === Arca LED Control Servlet Compilation ===
echo.

REM Define paths
set SERVLET_DIR=partitions\108a1490-8feb-4d0c-b3db-995dc5fc066c\opt\jetty-9-4-11\webapps\frontpanel\WEB-INF\classes
set SERVLET_FILE=LEDControlServlet.java
set JETTY_LIB=partitions\108a1490-8feb-4d0c-b3db-995dc5fc066c\opt\jetty-9-4-11\lib

REM Check if servlet source exists
if not exist "%SERVLET_DIR%\%SERVLET_FILE%" (
    echo ERROR: Servlet source file not found: %SERVLET_DIR%\%SERVLET_FILE%
    exit /b 1
)

echo Found servlet source: %SERVLET_DIR%\%SERVLET_FILE%

REM Find servlet-api JAR
set SERVLET_API_JAR=
for %%f in ("%JETTY_LIB%\servlet-api*.jar" "%JETTY_LIB%\javax.servlet-api*.jar") do (
    if exist "%%f" set SERVLET_API_JAR=%%f
)

if "%SERVLET_API_JAR%"=="" (
    echo WARNING: servlet-api JAR not found in %JETTY_LIB%
    echo Attempting compilation without explicit classpath...
    set CLASSPATH=
) else (
    echo Found servlet API: %SERVLET_API_JAR%
    set CLASSPATH=-cp "%SERVLET_API_JAR%"
)

REM Compile servlet
echo.
echo Compiling servlet...
cd /d "%SERVLET_DIR%"

javac %CLASSPATH% "%SERVLET_FILE%"
if %ERRORLEVEL% EQU 0 (
    echo.
    echo [SUCCESS] Compilation successful!
    echo.
    echo Generated files:
    dir /b LEDControlServlet*.class 2>nul
    echo.
    echo Next steps:
    echo 1. Verify web.xml has servlet registration
    echo 2. Deploy modified partition to SD card
    echo 3. Regenerate MD5 checksums
    echo 4. Boot device
    echo.
    echo See DEPLOYMENT_GUIDE.md for detailed instructions
) else (
    echo.
    echo [FAILED] Compilation failed!
    echo.
    echo Troubleshooting:
    echo 1. Ensure Java JDK is installed (javac command)
    echo 2. Check servlet-api JAR exists in Jetty lib directory
    echo 3. Verify servlet source code syntax
    echo.
    exit /b 1
)