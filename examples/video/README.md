examples/video
==============

Active Object layer (`camera_sys.h`). The top button starts, and stops, thirty seconds of video with sound to the microSD, with a report every second and a clap test for the sound's offset.

Nothing here records or takes a photo by itself: it waits for a button press. Plug the module in with the Dot off.

```bash
idf.py -C examples/video build
idf.py -C examples/video -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
