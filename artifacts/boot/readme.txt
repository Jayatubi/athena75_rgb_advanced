Boot splashes for host_tool `boot install <name>`.

    athena.qgf    19 frames, 1.9 s  the Athena wordmark the stock firmware
                                    carries as its built-in splash (gfx_boot)
    kbdfans.qgf   32 frames, 1.5 s  the KBDFANS logo the stock vial keymap
                                    actually plays (BOOTGIF = gfx_boot2)

Both were lifted out of the stock firmware's compiled-in graphics with

    python3 keyboards/ydkb/athena75_rgb_advanced/tools/qgf_from_c.py \
        <stock>/keyboards/ydkb/athena75_rgb/gfx/boot.qgf.c  -o artifacts/boot/athena.qgf
    python3 keyboards/ydkb/athena75_rgb_advanced/tools/qgf_from_c.py \
        <stock>/keyboards/ydkb/athena75_rgb/gfx/boot2.qgf.c -o artifacts/boot/kbdfans.qgf

The GIF previews in keyboards/ydkb/athena75_rgb_advanced/docs/boot/ are these
two files decoded by the same rules the firmware plays them with:

    python3 keyboards/ydkb/athena75_rgb_advanced/tools/qgf_preview.py \
        artifacts/boot/athena.qgf -o keyboards/ydkb/athena75_rgb_advanced/docs/boot/athena.gif

private/ is not tracked: that is where your own splashes go, built with
tools/make_boot_anim.py. They install by name just the same.
