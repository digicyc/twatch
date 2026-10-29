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

- The screen turns off after 10 s without input; tap it or press PWR or BOOT to wake it. A tap or
  PWR wakes it on the screen you left (the waking tap doesn't press anything).
- **BOOT** goes straight back to the watch face from any app or the Apps page, waking the screen
  if needed. It does nothing while an alarm is ringing, so Snooze/Stop stay on screen.
- Swipe up on the watch face for Apps. **Alarms** lists your alarms (up to 8): tap one to edit
  or delete it, use its switch to turn it on/off, and tap **+** to add one. In the editor, pick
  repeat days (or tap Weekdays / Weekends / Daily); with no days selected the alarm rings once and
  then switches itself off. Alarms wake the screen, vibrate and beep, and stop after 1 minute;
  **Snooze** waits 5 minutes.
- Alarms are saved in flash (NVS) and survive reboots and reflashing.
- **Messages** talks to MeshCore nodes over LoRa (USA/Canada preset, 910.525 MHz). It lists the
  Public channel and a chat for each MeshCore companion that has advertised nearby, with unread
  counts. New messages buzz and wake the screen. Tap a chat to read it and the pencil button to
  reply: pick a quick reply, or **Type a message** for a slide-up keyboard (one-shot shift,
  **1#** for numbers and symbols, ✓ to send). Direct messages show Sending / Sent / Delivered /
  Failed. Tap the signal icon in the list to advertise this watch so others can add it. The
  **This watch** card at the bottom of the list shows the name others see (TWatch-XXXX by
  default); tap it to rename the watch, which also re-advertises it. Chats are saved on the FFat
  partition.
- **GPS** has an on/off switch (saved across reboots; off cuts the receiver's power to save
  battery), the current position, altitude, speed and HDOP, and a bar per satellite showing signal
  strength (green = used in the fix). While GPS is on, the watch face shows a satellite icon at the top
  right: gray with the satellites in view while searching, green with the satellites used once it
  has a fix.
- **SD Card** shows the card's size and free space, a **Test read & write** button (writes 256 KB,
  reads it back, checks it, deletes it, and reports speeds), and a file browser: tap folders to
  open them and text files to view their first 2 KB. Cards must be FAT32; if the card can't be read,
  **Format card** erases it and formats it FAT32. Inserting or removing the card while the app is
  open remounts it.

If uploading fails or the USB port keeps reconnecting, enter download mode: hold **BOOT**,
press **RST**, release **BOOT**, upload, then press **RST** to run.

BOOT is the bottom left button on the Watch.

`build/compile_commands.json` can be used by clangd for editor completion.
