# WatchApp

Arduino sketch for the LilyGo T-Watch Ultra, built with `arduino-cli`, ESP32 core 3.3.10,
`../LilyGoLib`, and `../LilyGoLib-ThirdParty`.

```sh
make build              # compile into ./build
make upload             # flash (auto-detects /dev/ttyACM*, or pass PORT=...)
make monitor            # serial console at 115200
make flash              # build + upload + monitor
make RADIO=Radio_LR1121 build   # if your watch has a different radio than SX1262
```

If uploading fails or the USB port keeps reconnecting, enter download mode: hold **BOOT**,
press **RST**, release **BOOT**, upload, then press **RST** to run.

BOOT is the bottom left button on the Watch.

`build/compile_commands.json` can be used by clangd for editor completion.
