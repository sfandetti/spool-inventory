# Spool Display (round desk display)

Firmware for the Waveshare **ESP32-S3-Touch-AMOLED-1.75** (466×466 CO5300 AMOLED, CST9217 touch).
Swipe left/right for home and one screen per material, up/down for that material's rolls.

Build, flash and setup instructions are in the main README:
[Optional: the round desk display](../README.md#optional-the-round-desk-display).

Wi-Fi and the server address are not in the code. They are sent over USB from the Spool Inventory page
(**Round display → Set up over USB**) and stored in the display's flash (ESP32 `Preferences`).
