examples/console_actions
========================

Active Object layer (`camera_sys.h`). The camera on kodeOS's console: fifteen `cam_` functions (photos, focus, lens, standby, the looks, a movie) and ten variables.

Nothing here records or takes a photo by itself: it waits for a command typed on the console. Plug the module in with the Dot off.

```bash
idf.py -C examples/console_actions build
idf.py -C examples/console_actions -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
