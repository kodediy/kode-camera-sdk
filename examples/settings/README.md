examples/settings
=================

Task layer (`camera.h`). The top button starts a tour of the settings: each with its range, default and value, then one photo of each look saved to the microSD.

Nothing here records or takes a photo by itself: it waits for a button press. Plug the module in with the Dot off.

```bash
idf.py -C examples/settings build
idf.py -C examples/settings -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
