examples/module
===============

Task layer (`camera.h`). The camera found on J3 and what the owner knows of it: the board, which way round, the frame rate and the clock, where its lines arrive on J3. Noticed when it leaves. Run it first; it needs no card.

Nothing here records or takes a photo: it only reports, in the log, every five seconds. Plug the module in with the Dot off.

```bash
idf.py -C examples/module build
idf.py -C examples/module -p PORT flash
```

Docs: https://docs.kode.diy/modules/camera
