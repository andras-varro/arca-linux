#!/bin/bash
# Servlet Compilation Script for Arca LED Control

set -e  # Exit on error

echo "=== Arca LED Control Servlet Compilation ==="
echo ""

# Define paths
SERVLET_DIR="partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes"
SERVLET_FILE="LEDControlServlet.java"
JETTY_LIB="partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/lib"

# Check if servlet source exists
if [ ! -f "$SERVLET_DIR/$SERVLET_FILE" ]; then
    echo "ERROR: Servlet source file not found: $SERVLET_DIR/$SERVLET_FILE"
    exit 1
fi

echo "Found servlet source: $SERVLET_DIR/$SERVLET_FILE"

# Find servlet-api JAR
SERVLET_API_JAR=$(find "$JETTY_LIB" -name "servlet-api*.jar" -o -name "javax.servlet-api*.jar" | head -1)

if [ -z "$SERVLET_API_JAR" ]; then
    echo "WARNING: servlet-api JAR not found in $JETTY_LIB"
    echo "Attempting compilation without explicit classpath..."
    CLASSPATH=""
else
    echo "Found servlet API: $SERVLET_API_JAR"
    CLASSPATH="-cp $SERVLET_API_JAR"
fi

# Compile servlet
echo ""
echo "Compiling servlet..."
cd "$SERVLET_DIR"

# Use absolute path for classpath
ABS_CLASSPATH=""
if [ -n "$SERVLET_API_JAR" ]; then
    ABS_CLASSPATH="-cp $(cd "$(dirname "$SERVLET_API_JAR")" && pwd)/$(basename "$SERVLET_API_JAR")"
fi

echo "Using classpath: $ABS_CLASSPATH"
echo ""

if javac $ABS_CLASSPATH "$SERVLET_FILE"; then
    echo ""
    echo "✓ Compilation successful!"
    echo ""
    echo "Generated files:"
    ls -lh LEDControlServlet*.class 2>/dev/null || echo "  LEDControlServlet.class"
    echo ""
    echo "Next steps:"
    echo "1. Verify web.xml has servlet registration"
    echo "2. Deploy modified partition to SD card"
    echo "3. Regenerate MD5 checksums"
    echo "4. Boot device"
    echo ""
    echo "See DEPLOYMENT_GUIDE.md for detailed instructions"
else
    echo ""
    echo "✗ Compilation failed!"
    echo ""
    echo "Troubleshooting:"
    echo "1. Ensure Java JDK is installed (javac command)"
    echo "2. Check servlet-api JAR exists in Jetty lib directory"
    echo "3. Verify servlet source code syntax"
    echo ""
    exit 1
fi