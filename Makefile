# Build, flash, and monitor WatchApp with arduino-cli.
# Board settings mirror LilyGoLib's docs/CI for the T-Watch Ultra (ESP32 core 3.3.10).

RADIO ?= Radio_SX1262   # Radio_SX1262 | Radio_SX1280 | Radio_CC1101 | Radio_LR1121 | Radio_SI4432
PORT  ?= $(firstword $(wildcard /dev/serial/by-id/usb-Espressif*) $(wildcard /dev/ttyACM*))
BAUD  ?= 115200

ROOT  := $(abspath ..)
BUILD ?= build

# MeshCore radio and table settings. They are global because the MeshCore and RadioLib headers
# read them; the radio values are MeshCore's "USA/Canada (Recommended)" preset.
MESH_FLAGS := -DRADIOLIB_STATIC_ONLY=1 -DRADIOLIB_GODMODE=1 \
	-DLORA_FREQ=910.525 -DLORA_BW=62.5 -DLORA_SF=7 -DLORA_CR=5 -DLORA_TX_POWER=22 \
	-DSX126X_DIO3_TCXO_VOLTAGE=1.8f -DSX126X_DIO2_AS_RF_SWITCH=true -DSX126X_CURRENT_LIMIT=140 \
	-DSX126X_RX_BOOSTED_GAIN=1 -DMAX_CONTACTS=64 -DMAX_GROUP_CHANNELS=4
EXTRA_FLAGS ?=
FQBN  := esp32:esp32:twatch_ultra:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,Revision=$(strip $(RADIO))

.PHONY: deps build upload monitor flash settime ports clean

# Arduino libraries MeshCore needs beyond LilyGoLib-ThirdParty.
deps:
	arduino-cli lib install "Crypto@0.4.0" "base64@1.3.0"

build:
	arduino-cli compile --fqbn '$(FQBN)' --build-path $(BUILD) \
		--build-property "compiler.cpp.extra_flags=$(MESH_FLAGS) $(EXTRA_FLAGS)" \
		--library $(ROOT)/LilyGoLib --libraries $(ROOT)/LilyGoLib-ThirdParty --library lib/MeshCore .

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
