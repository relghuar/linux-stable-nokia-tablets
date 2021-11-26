
# Linux kernel for Nokia N810

This branch is based on linux-stable-rc 5.15.x, with additional changes for Nokia N810 internet tablet device (possibly N800 as well?).
Eventual goal is to get as much as possible patched into the upstream kernel.
Code quality of most changes is awful; main goal for now is to get the hardware working.

## Working:
- LCD panel using old omap2fb system, no DRM yet
  - several components require no-reset-on-init hwmod flag to work properly
- LCD backlight included in panel driver, WiP on a separate Tahvo PWM-LED driver
- touchscreen
- keyboard with backlight, lm8323 driver adjusted for dt-bindings
  - whole driver should be rewritten using matrix-keypad framework
- wifi
  - depends on nasty workaround for mcspi fifo dma problem; seems related to https://www.spinics.net/lists/linux-omap/msg147996.html although patch for that is already upstreamed (https://lore.kernel.org/lkml/20190115065832.23705-1-vigneshr@ti.com/). It's possible omap2420 has a different issue. Difficult to fix without full OMAP2420 TRM.

## Next steps:
- remove LCD panel driver dependency on direct Retu/Tahvo access
  - backlight should be done using new tahvo-led + led-bl drivers
  - Vtornado power for S1D13745 probably needs some kind of tahvo regulator driver
  - patches for new tahvo-led and tahvo-reg drivers should be reasonably trivial to upstream
- try to upstream lm8323 driver patches for dt-bindings and standard matrix-keypad
- move RFBI and N8x0 panel driver to drm/gpu
  - trying to upstream RFBI might be problem without other testable platforms?
- find out correct init sequences for LCD panel and framebuffer so the no-reset hwmod flags can be removed
- start adding retu/tahvo drivers for battery management ; because of lack of documentation of both chips, best source will probably be https://git.openwrt.org/?p=openwrt/openwrt.git;a=tree;f=target/linux/omap24xx/patches-3.3;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e
- fix mcspi fifo dma issue for 16-bit spi transfers (no idea where to start without SoC docs)

