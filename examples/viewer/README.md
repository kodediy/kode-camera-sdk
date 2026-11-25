examples/viewer
===============

Active Object layer (`camera_sys.h`). The camera on the whole screen with LVGL: the pad zooms and mirrors, and a touch focuses where it lands.

Nothing here records or takes a photo by itself: it waits for a button press or a touch. Plug the module in with the Dot off.

```bash
idf.py -C examples/viewer build
idf.py -C examples/viewer -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
