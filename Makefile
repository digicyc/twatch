# Build, flash, and monitor WatchApp with arduino-cli.
# Board settings mirror LilyGoLib's docs/CI for the T-Watch Ultra (ESP32 core 3.3.10).

RADIO ?= Radio_SX1262   # Radio_SX1262 | Radio_SX1280 | Radio_CC1101 | Radio_LR1121 | Radio_SI4432
PORT  ?= $(firstword $(wildcard /dev/serial/by-id/usb-Espressif*) $(wildcard /dev/ttyACM*))
BAUD  ?= 115200

ROOT  := $(abspath ..)
BUILD := build
FQBN  := esp32:esp32:twatch_ultra:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,Revision=$(strip $(RADIO))

.PHONY: build upload monitor flash settime ports clean

build:
	arduino-cli compile --fqbn '$(FQBN)' --build-path $(BUILD) \
		--library $(ROOT)/LilyGoLib --libraries $(ROOT)/LilyGoLib-ThirdParty .

upload:
	@test -n "$(PORT)" || { echo "No watch serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	arduino-cli upload --fqbn '$(FQBN)' --input-dir $(BUILD) --port $(PORT) .

monitor:
	@test -n "$(PORT)" || { echo "No watch serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	arduino-cli monitor --port $(PORT) --config baudrate=$(BAUD)

flash: build upload monitor

# Sends the host's current Unix time to the running sketch, which sets its RTC.
settime:
	@test -n "$(PORT)" || { echo "No watch serial port found; plug it in or pass PORT=/dev/ttyACMx"; exit 1; }
	stty -F $(PORT) raw -echo && printf 'T%s\n' "$$(date +%s)" > $(PORT)

ports:
	arduino-cli board list

clean:
	rm -rf $(BUILD)
