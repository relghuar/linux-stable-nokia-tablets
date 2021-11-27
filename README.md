
Linux kernel for Nokia N810
===========================

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
- RGB LED in top left corner, working almost out-of-the-box with upstream lp5521 driver, just needed correct dt-bindings
- wifi
  - depends on nasty workaround for mcspi fifo dma problem, see [bellow](#wifi)

## Next steps:
- remove LCD panel driver dependency on direct Retu/Tahvo access
  - backlight should be done using new tahvo-led + led-bl drivers (WiP - first patch already in place)
  - Vtornado power for S1D13745 probably needs some kind of regulator driver as tahvo mfd cell
  - patches for new tahvo-led and tahvo-reg drivers should be reasonably trivial to upstream
- try to upstream lm8323 driver patches for dt-bindings and standard matrix-keypad
- move RFBI and N8x0 panel driver to drm/gpu
  - trying to upstream RFBI **might** be a problem without other testable platforms, as well as practically no documentation for OMAP2420 itself...? :-)
  - 5.15 kernel is already quite big with at least basic modern features included. With basic debug symbols to produce readable stacktraces (quite necessary for development), it's currently ~10k under the 2MB limit imposed by NOLO. Not sure if drm won't push it over.
- find out correct init sequences for LCD panel and framebuffer so the no-reset hwmod flags can be removed from gpio1 and all the dss modules (this is also necessary for proper standby mode most likely?)
- start adding retu/tahvo drivers for battery management
  - because of lack of documentation of both chips, best source will probably be latest reasonable [openwrt patches for v3.3 kernel](https://git.openwrt.org/?p=openwrt/openwrt.git;a=tree;f=target/linux/omap24xx/patches-3.3;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e), as well as original (even more ancient) [vendor sources](http://repository.maemo.org/pool/maemo4.1.2/free/k/kernel-source-diablo/)
  - first step will probably be some debug readout (sysfs?) of all registers from both retu and tahvo chips
- fix mcspi fifo dma issue for 16-bit spi transfers to have wifi working without the hack (no idea where to start without SoC docs)

# Hardware notes

## Backlight

There are 2 kinds of backlights on N810:
- **keyboard backlight** driven by PWM_0 output of lm8323 keyboard chip ; this one works fine. There are 3 such pwm outputs, and according to vendor kernel there should be another backlight connected to PWM_1 (named "cover" in board init code), however both PWM_1 and PWM_2 seems to be unconnected according to rx-44 schematics and actually trying to use it on real N810 has no visible effect. Most probably there is only one backlight available, for the main keyboard.
- **LCD backlight** provided by PWM300 output of the Betty/Tahvo chip ; this is controlled by one of the registers in tahvo mfd and allows for 127 levels of brightness. There is already a leds-tahvo.c driver in this branch which can be attached to led-backlight driver (and actually is in the n810 dts). Backlight control is still included directly in the panel driver as well, this needs removed and probably replaced by led_bl to allow for proper console blanking. tahvo-ledpwm is quite simple driver that should not be difficult to get accepted upstream, after the main retu-mfd extension.

## RTC

According to the N810 schematics, there are several issues here:
- Backup battery (somewhere on-board probably?) seems to only be connected to Vilma chip, pin Vback, so no other RTC source can survive removal of main battery.
- Menelaus RTC driver present in kernel is useless without some changes - the 32KDETEN pin on the chip is pulled down, so menelaus_rtc_init() breaks off right at the first check. According to schematics, Menelaus chip does not seem to have any 32Khz input (there's only 32KOUT which is not connected anywhere). I didn't find any datasheet yet, so it's difficult to say if a pulled-down 32KDETEN makes RTC truly unusable ; or generally if this RTC could be made to work in N810.
- Vilma/Retu RTC driver built based on [this old patch file](https://git.openwrt.org/?p=openwrt/openwrt.git;a=blob;f=target/linux/omap24xx/patches-3.3/250-cbus.patch;h=b152a00b5b986b780be8d64443032d7c25e0594a;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e) as well as original vendor kernel sources from [here](http://repository.maemo.org/pool/maemo4.1.2/free/k/kernel-source-diablo/) seems to work at least partially:
  - DSR/HMR registers only hold days, hours, minutes and seconds, and event that is severely limited. Vendor kernel does not even try to write any reasonable values there, it simply writes "mostly-0" to DSR and suggests this should reset both counters to 0. No idea yet what would happen by writing HMR, so far it had no discernible effect.
  - Resetting whole RTC seemed to work fine at least for timekeeping part, DSR/HMR started counting from 0.
  - There is alarm functionality included, seems to be a single hour+minute register so only one alarm can be active at a time. If it's really not possible to set HMR time register
  - TODO: next time I have the device disassembled, check RTC battery status (close to mini-SD slot according to pcb drawing) - it's ancient, and the device survived an actual flood, so it's likely dead...

## Wifi

The p54spi driver uses 16-bit spi protocol, which apparently does not play well with OMAP2420's mcspi/fifo/dma configuration in current kernel, at least for 2-byte transfer. Perhaps it's for any non-8-bit mode, although LCD display uses 10bit transfers in some situations as well and it seems to work. Maybe it does not use spi fifo as it only transfers 3-4 bytes at a time, or there is some difference between 10 and 16 bit mode I'm not aware of. I'm not yet familiar enough with omap2 internals to really tell - it's also possible spi/fifo/dma combination is only usable for 8-bit modes on the omap2420; there's nothing to do here really until I get my hands on the right TRM.
There was a similar issue reported for N900 (OMAP3530) some years ago [here](https://www.spinics.net/lists/linux-omap/msg147996.html) - exactly the same symptoms, EOW and TXFFE timeout reports after 1s delays. This was apparently fixed [here](https://lore.kernel.org/lkml/20190115065832.23705-1-vigneshr) and I found the relevant fix in the code, though for some reason this fix does not work for N810.
The only way I managed to get wifi working was to reduce wait times for EOW and TXFFE bits in mcspi driver to 10ms and simply ignore any timeouts. Wifi comes up like this, and runs stably at around 50kbps speeds. Definitely not a long-term solution, but enough for further development.
I've started porting p54spi itself to OF probing - it works already and has been removed from the board init code, however it still uses module parameters for GPIOs, as well as probably some other hardcoded values which could all be moved to devicetree.

## General Retu/Tahvo topics

Any reasonable new drivers for lcd backlight, framebuffer power control and general battery management will need additional patching of main retu-mfd driver, to allow mfd cells registering from dt-bindings. This has already been started for the tahvo-ledpwm driver, however it should probably be extended to allow full mfd_cell capabilities like id, reg address, resources and such. At the very least it should support irq resources as those are already used in existing cell drivers like retu-powerbtn, and will for sure be necessary for charging control.


