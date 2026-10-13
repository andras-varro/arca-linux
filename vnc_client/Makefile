# VNC Client for Arca - Makefile
# Cross-compilation for ARM Cortex-A8

# Toolchain configuration
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
STRIP = $(CROSS_COMPILE)strip

# Project directories
COMMON_DIR = ../native_apps/common
BUILD_DIR = build
DEPS_DIR = deps

# Target binary
TARGET = vnc_client
TARGET_STRIPPED = $(TARGET)_stripped

# Source files
SRCS = vnc_client.c \
       vnc_renderer.c \
       vnc_input.c \
       vnc_settings.c \
       vnc_pad.c \
       $(COMMON_DIR)/framebuffer.c \
       $(COMMON_DIR)/touch_input.c \
       $(COMMON_DIR)/hardware.c \
       $(COMMON_DIR)/config.c \
       $(COMMON_DIR)/input_scan.c \
       $(COMMON_DIR)/ui_focus.c \
       $(COMMON_DIR)/logger.c

# Object files
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

# Compiler flags
CFLAGS = -O2 \
         -march=armv7-a \
         -mfpu=neon \
         -Wall \
         -Wextra \
         -I. \
         -I$(COMMON_DIR) \
         -I$(DEPS_DIR)/include

# Linker flags - static linking
LDFLAGS = -static \
          $(DEPS_DIR)/lib/libvncclient.a \
          $(DEPS_DIR)/lib/libz.a \
          $(DEPS_DIR)/lib/libjpeg.a \
          -lpthread \
          -lm

# Build rules
.PHONY: all clean deps install deploy

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/$(COMMON_DIR)

$(BUILD_DIR)/%.o: %.c
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "LD $@"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "STRIP $@"
	@$(STRIP) $(TARGET) -o $(TARGET_STRIPPED)
	@echo "Build complete: $(TARGET_STRIPPED)"
	@ls -lh $(TARGET_STRIPPED)

# Build dependencies (LibVNCClient, zlib, libjpeg)
deps:
	@echo "Building dependencies..."
	@mkdir -p $(DEPS_DIR)
	@./build-deps.sh

# Install to local system (for testing)
install: $(TARGET)
	@echo "Installing to /opt/vnc_client..."
	@sudo mkdir -p /opt/vnc_client
	@sudo cp $(TARGET_STRIPPED) /opt/vnc_client/vnc_client
	@sudo chmod +x /opt/vnc_client/vnc_client
	@echo "Installed to /opt/vnc_client/vnc_client"

# Deploy to Arca device
deploy: $(TARGET)
	@if [ -z "$(DEVICE_IP)" ]; then \
		echo "Error: DEVICE_IP not set. Usage: make deploy DEVICE_IP=192.168.50.73"; \
		exit 1; \
	fi
	@echo "Deploying to $(DEVICE_IP)..."
	@ssh root@$(DEVICE_IP) "mkdir -p /opt/vnc_client"
	@scp $(TARGET_STRIPPED) root@$(DEVICE_IP):/opt/vnc_client/vnc_client
	@ssh root@$(DEVICE_IP) "chmod +x /opt/vnc_client/vnc_client"
	@echo "Deploying config file..."
	@scp vnc_client.conf root@$(DEVICE_IP):/opt/vnc_client/vnc_client.conf
	@echo "Deployed successfully"
	@echo "Run with: ssh root@$(DEVICE_IP) '/opt/vnc_client/vnc_client'"

# Clean build artifacts
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(TARGET) $(TARGET_STRIPPED)
	@echo "Clean complete"

# Clean everything including dependencies
distclean: clean
	@echo "Cleaning dependencies..."
	@rm -rf $(DEPS_DIR)
	@echo "Distclean complete"

# Help target
help:
	@echo "VNC Client for Arca - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build the VNC client (default)"
	@echo "  deps       - Build dependencies (LibVNCClient, zlib, libjpeg)"
	@echo "  clean      - Remove build artifacts"
	@echo "  distclean  - Remove build artifacts and dependencies"
	@echo "  install    - Install to local /opt/vnc_client"
	@echo "  deploy     - Deploy to Arca device"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make deps                              # First time setup"
	@echo "  make                                   # Build"
	@echo "  make deploy DEVICE_IP=192.168.50.73   # Deploy to device"
	@echo ""
	@echo "Environment Variables:"
	@echo "  CROSS_COMPILE - Toolchain prefix (default: arm-linux-gnueabihf-)"
	@echo "  DEVICE_IP     - Arca IP address for deployment"
