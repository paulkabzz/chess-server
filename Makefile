# ==============================================================================
# Cross-Platform Makefile for STM32 Chess Server
# ==============================================================================

# 1. Detect OS and set CPU core count
ifeq ($(OS),Windows_NT)
    DETECTED_OS := Windows
    CORES := $(NUMBER_OF_PROCESSORS)
    PORT ?= COM3
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        DETECTED_OS := macOS
        CORES := $(shell sysctl -n hw.ncpu)
        PORT ?= $(firstword $(wildcard /dev/cu.usbmodem* /dev/tty.usbmodem* /dev/cu.usbserial*))
    else
        DETECTED_OS := Linux
        CORES := $(shell nproc)
        PORT ?= $(firstword $(wildcard /dev/ttyACM* /dev/ttyUSB*))
    endif
endif

.PHONY: help check free gui start_gui test clean

help:
	@echo "Available targets:"
	@echo "  check     - Check if the serial port (detected: $(PORT)) is in use"
	@echo "  free      - Kill the process holding the serial port"
	@echo "  gui       - Build the Qt 6 C++ GUI"
	@echo "  start_gui - Run the built GUI"
	@echo "  test      - Run the automated pytest suite"
	@echo "  clean     - Remove build artifacts"
	@echo ""
	@echo "Usage: make <target> [PORT=/your/specific/port]"

# ------------------------------------------------------------------------------
# CHECK: Find out what is using the serial port
# ------------------------------------------------------------------------------
check:
ifeq ($(DETECTED_OS),Windows)
	@echo "Windows: Auto-check is not supported. Please check Device Manager or close the app holding $(PORT)."
else
	@if [ -z "$(PORT)" ]; then \
		echo "Error: No serial port detected. Please specify manually, e.g., 'make check PORT=/dev/ttyACM0'"; \
	elif lsof $(PORT) > /dev/null 2>&1; then \
		echo "Port $(PORT) is IN USE by:"; \
		lsof $(PORT); \
	else \
		echo "Port $(PORT) is free."; \
	fi
endif

# ------------------------------------------------------------------------------
# FREE: Kill the process holding the port (e.g., if the GUI crashed)
# ------------------------------------------------------------------------------
free:
ifeq ($(DETECTED_OS),Windows)
	@echo "Windows: Please manually close any open Serial Monitor or GUI."
else
	@if [ -z "$(PORT)" ]; then \
		echo "Error: No serial port detected."; \
	else \
		PIDS=$$(lsof -t $(PORT) 2>/dev/null); \
		if [ -n "$$PIDS" ]; then \
			echo "Killing process(es) holding $(PORT)..."; \
			echo $$PIDS | xargs kill -9; \
			echo "Port $(PORT) freed."; \
		else \
			echo "Port $(PORT) is already free."; \
		fi \
	fi
endif

# ------------------------------------------------------------------------------
# GUI: Build the application
# ------------------------------------------------------------------------------
gui:
	@echo "Building GUI using $(CORES) cores on $(DETECTED_OS)..."
	mkdir -p build && cd build && cmake .. && make -j$(CORES)

# ------------------------------------------------------------------------------
# START_GUI: Run the application (passing the port automatically)
# ------------------------------------------------------------------------------
start_gui:
	@echo "Starting GUI (Port: $(PORT))..."
	@if [ -n "$(PORT)" ]; then \
		./build/chess_gui $(PORT); \
	else \
		./build/chess_gui; \
	fi

# ------------------------------------------------------------------------------
# TEST: Run the pytest suite (ensures GUI is not holding the port!)
# ------------------------------------------------------------------------------
test:
	@echo "Running pytest suite on port $(PORT)..."
	@echo "Note: Make sure the GUI is closed before running tests."
ifeq ($(DETECTED_OS),Windows)
	@set SERIAL_PORT=$(PORT) && python3 -m pytest -v test/test_chess.py
else
	@SERIAL_PORT=$(PORT) python3 -m pytest -v test/test_chess.py
endif

# ------------------------------------------------------------------------------
# CLEAN: Remove build artifacts
# ------------------------------------------------------------------------------
clean:
	@echo "Cleaning build artifacts..."
	rm -rf build/
	rm -rf __pycache__/
	rm -rf test/__pycache__/
	rm -rf .pytest_cache/
