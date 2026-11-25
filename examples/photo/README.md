examples/photo
==============

Active Object layer (`camera_sys.h`). The top button takes an 800 x 600 photo to the microSD (`/sdcard/photo.jpg`); a short press of the bottom one takes a 5 MP photo after a focus pass (`/sdcard/photo_5mp_N.jpg`). Holding the bottom button goes back to kodeOS.

Nothing here records or takes a photo by itself: it waits for a button press. Plug the module in with the Dot off.

```bash
idf.py -C examples/photo build
idf.py -C examples/photo -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
