# ALIENTEK ATK-DNESP32S3-BOX

ESP32-S3 (16 MB flash, 8 MB octal PSRAM), 2.4" 320x240 ST7789 on an 8080 bus, ES8311
or NS4168 audio, XL9555 IO expander, optional CHSC5432 touch panel, TF card slot.

## Variants

| Variant | Description |
| --- | --- |
| `atk-dnesp32s3-box` | Xiaozhi chat UI only. |
| `atk-dnesp32s3-box-desktop` | Touch desktop that hosts the Xiaozhi chat as one app next to Camera, Video, Photos and Music. Ported from the ALIENTEK factory demo. Needs the CHSC5432 touch panel. |

```sh
python3 scripts/build.py alientek/atk-dnesp32s3-box --name atk-dnesp32s3-box-desktop
```

## Desktop variant

The board boots into the chat screen so that Wi-Fi setup, activation codes and upgrades
are visible, then switches to the desktop once Xiaozhi is idle.

- **Chat**: the regular Xiaozhi UI. The wake word, the BOOT button or a server-side event
  bring it to the front automatically (closing any open app); after a conversation that
  interrupted the desktop it returns there after a few seconds of idle.
- **Camera**: live preview from a USB UVC camera plugged into the USB port. The round
  button saves a JPEG to `/sdcard/PICTURE/IMG_nnnn.jpg`. The same camera is used by the
  AI's `take_photo` tool.
- **Video**: AVI files with MJPEG video and 16-bit PCM audio from `/sdcard/VIDEO`.
  Frames are scaled to the screen; late frames are dropped to stay in sync with the sound.
- **Photos**: JPG, PNG (up to 800x600) and BMP from `/sdcard/PICTURE`. Swipe or use the
  arrows.
- **Music**: MP3, WAV, AAC, M4A and FLAC from `/sdcard/MUSIC`.

The floating home button can be dragged out of the way.

### TF card layout

```
/sdcard
├── MUSIC/     *.mp3 *.wav *.aac *.m4a *.flac
├── PICTURE/   *.jpg *.jpeg *.png *.bmp   (camera snapshots are saved here too)
└── VIDEO/     *.avi (MJPEG + PCM)
```

Convert videos with, for example:

```sh
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease" -r 20 \
  -c:v mjpeg -q:v 6 -c:a pcm_s16le -ar 24000 -ac 1 output.avi
```

Using 24 kHz mono audio matches the codec and avoids resampling.

### Notes

- Media playback pauses the wake word. It resumes when playback stops.
- The USB port runs in host mode for the camera. Logs over the USB-Serial-JTAG console stop
  once the USB host starts, so use the UART pins (GPIO43/44) for logs. To flash, hold BOOT
  while resetting to enter download mode.
- With no camera connected, boot waits `CONFIG_USB_UVC_INIT_TIMEOUT_MS` (3 s) for one. The
  camera must be connected at boot.
- If the TF card was missing at boot, it is mounted when an app that needs it opens.
