# WatchApp

Arduino sketch for the LilyGo T-Watch Ultra, built with `arduino-cli`, ESP32 core 3.3.10,
`../LilyGoLib`, and `../LilyGoLib-ThirdParty`.

```sh
make build              # compile into ./build
make upload             # flash (auto-detects /dev/ttyACM*, or pass PORT=...)
make monitor            # serial console at 115200
make flash              # build + upload + monitor
make settime            # set the watch's RTC from this computer's clock (sketch must be running)
make RADIO=Radio_LR1121 build   # if your watch has a different radio than SX1262
```

## Using the watch

- The screen turns off after 10 s without input; tap it or press PWR or BOOT to wake it.
- Swipe up on the watch face for Apps. **Alarms** lists your alarms (up to 8): tap one to edit
  or delete it, use its switch to turn it on/off, and tap **+** to add one. In the editor, pick
  repeat days (or tap Weekdays / Weekends / Daily); with no days selected the alarm rings once and
  then switches itself off. Alarms wake the screen, vibrate and beep, and stop after 1 minute;
  **Snooze** waits 5 minutes.
- Alarms are saved in flash (NVS) and survive reboots and reflashing.

If uploading fails or the USB port keeps reconnecting, enter download mode: hold **BOOT**,
press **RST**, release **BOOT**, upload, then press **RST** to run.

BOOT is the bottom left button on the Watch.

`build/compile_commands.json` can be used by clangd for editor completion.
