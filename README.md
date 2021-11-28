
Linux kernel for Nokia N810
===========================

This branch is based on linux-stable-rc 5.15.x, with additional changes for Nokia N810 internet tablet device (possibly N800 as well?).
Eventual goal is to get as much as possible patched into the upstream kernel.
Code quality of most changes is awful; main goal for now is to get the hardware working.

## Working

- LCD panel using old omap2fb system, no DRM yet
  - gpio1 and dispc require no-reset-on-init hwmod flag to the panel work properly (gpio1 because of LCD_RESET, no idea why dispc)
- LCD backlight
- touchscreen
- keyboard with backlight, lm8323 driver adjusted for dt-bindings
  - whole driver should be rewritten using matrix-keypad framework
- RGB LED in top left corner, working almost out-of-the-box with upstream lp5521 driver, just needed correct dt-bindings
- wifi
  - depends on nasty workaround for mcspi fifo dma problem, see [bellow](#wifi)

## Not tested

- Bluetooth
- Full suspend/resume
- Audio


## Next steps:

- remove LCD panel driver dependency on direct Retu/Tahvo access
  - Vtornado power for S1D13745 probably needs some kind of regulator driver attached as tahvo mfd cell

- try to upstream lm8323 driver patches for dt-bindings and standard matrix-keypad

- move RFBI and N8x0 panel driver to drm/gpu
  - trying to upstream RFBI **might** be a problem without other testable platforms, as well as practically no documentation for OMAP2420 itself...? :-)
  - 5.15 kernel is already quite big with at least basic modern features included. With basic debug symbols to produce readable stacktraces (quite necessary for development), it's currently ~10k under the 2MB limit imposed by NOLO. Not sure if drm won't push it over.

- find out correct init sequences for LCD panel and framebuffer so the no-reset hwmod flags can be removed from gpio1 and dispc modules (this is also necessary for proper standby mode most likely?)

- start adding retu/tahvo drivers for battery management
  - because of lack of documentation of both chips, best source will probably be latest reasonable [openwrt patches for v3.3 kernel](https://git.openwrt.org/?p=openwrt/openwrt.git;a=tree;f=target/linux/omap24xx/patches-3.3;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e), as well as original (even more ancient) [vendor sources](http://repository.maemo.org/pool/maemo4.1.2/free/k/kernel-source-diablo/)
  - first step will probably be some debug readout (sysfs?) of all registers from both retu and tahvo chips

- fix mcspi fifo dma issue for 16-bit spi transfers to have wifi working without the hack (no idea where to start without SoC docs)


# Hardware notes

## LCD

The display is composed of two parts:
- panel itself, a Sharp LS041Y3 connected to SPI1 using MIPID command protocol and LCD_RESET signal
- external framebuffer Epson S1D13745 connected to the RFBI interface (DSS DBI), using both LCD_RESET and POWERDOWN signals

The powerdown seems to work fine (power consumption has to be investigated), pulling it down on display_disable still allows for successful wakeup.

Reset is tricky, pulling it down either on blank/suspend or by powerup hwmod reset renders the display unusable (it simply stays black). It is currently not clear if the issue is with framebuffer or panel or both. **This needs further investigation.**
To work around this for now, gpio1 hwmod reset-on-init has to be skipped to keep it up from NOLO, and we have to avoid pulling it down during blank or standby.
Another module needing no-reset-on-init is dss **dispc** ; although funny enough both **dss** and **rfbi** can be reset just fine. Another point to investigate, there must be something missing in dss initialization sequence (possibly in rfbi?).

## Backlight

There are 2 kinds of backlights on N810:

- **keyboard backlight** driven by PWM_0 output of lm8323 keyboard chip ; this one works fine. There are 3 such pwm outputs, and according to vendor kernel there should be another backlight connected to PWM_1 (named "cover" in board init code), however both PWM_1 and PWM_2 seem to be unconnected according to rx-44 schematics and actually trying to use it on real N810 has no visible effect. Most probably there is only one backlight available, for the main keyboard.

- **LCD backlight** provided by PWM300 output of the Betty/Tahvo chip ; this is controlled by one of the registers in tahvo mfd and allows for 127 levels of brightness. There is already a leds-tahvo.c driver in this branch, attached to panel via standard led-backlight driver using dt-bindings. Blanking works, although power consumption is probably not ideal as there are still some suspend/resume operations missing for the framebuffer chip. The driver for tahvo-ledpwm is quite simple and should not be difficult to get accepted upstream, after patching OF bindings into retu-mfd driver (see [Retu/Tahvo](#retu-tahvo)).

## RTC

According to the N810 schematics, there are several issues here:

- Backup battery (somewhere on-board probably?) seems to only be connected to Vilma chip, pin Vback, so no other RTC source can survive removal of main battery.

- Menelaus RTC driver present in kernel is useless without some changes - the 32KDETEN pin on the chip is pulled down, so menelaus_rtc_init() breaks off right at the first check. According to schematics, Menelaus chip does not seem to have any 32Khz input (there's only 32KOUT which is not connected anywhere). I didn't find any datasheet yet, so it's difficult to say if a pulled-down 32KDETEN makes RTC truly unusable ; or generally if this RTC could be made to work in N810.

- Vilma/Retu RTC driver built based on [this old patch file](https://git.openwrt.org/?p=openwrt/openwrt.git;a=blob;f=target/linux/omap24xx/patches-3.3/250-cbus.patch;h=b152a00b5b986b780be8d64443032d7c25e0594a;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e) as well as original vendor kernel sources from [here](http://repository.maemo.org/pool/maemo4.1.2/free/k/kernel-source-diablo/) seems to work at least partially:
  - DSR/HMR registers only hold days, hours, minutes and seconds, and event that is severely limited. Vendor kernel does not even try to write any reasonable values there, it simply writes "mostly-0" to DSR and suggests this should reset both counters to 0. No idea yet what would happen by writing HMR, so far it had no discernible effect.
  - Resetting whole RTC seemed to work fine at least for timekeeping part, DSR/HMR started counting from 0.
  - There is alarm functionality included, seems to be a single hour+minute register so only one alarm can be active at a time. If it's really not possible to set HMR time register, using the alarm referenced to it will be quite cumbersome. RTC would of course be useless for timekeeping, and actual hour:minute of any alarm would have to be "converted" to corresponding rtc-relative time. This would probably have to be performed by the driver internally.
  - TODO: next time I have the device disassembled, check RTC battery status (close to mini-SD slot according to pcb drawing) - it's ancient, and the device survived an actual flood, so it's likely dead...

## Wifi

The p54spi driver uses 16-bit spi protocol, which apparently does not play well with OMAP2420's mcspi/fifo/dma configuration in current kernel, at least for 2-byte transfer. Perhaps it's for any non-8-bit mode, although LCD display uses 10bit transfers in some situations as well and it seems to work. Maybe it does not use spi fifo as it only transfers 3-4 bytes at a time, or there is some difference between 10 and 16 bit mode I'm not aware of. I'm not yet familiar enough with omap2 internals to really tell - it's also possible spi/fifo/dma combination is only usable for 8-bit modes on the omap2420; there's nothing to do here really until I get my hands on the right TRM.

There was a similar issue reported for N900 (OMAP3530) some years ago [here](https://www.spinics.net/lists/linux-omap/msg147996.html) - exactly the same symptoms, EOW and TXFFE timeout reports after 1s delays. This was apparently fixed [here](https://lore.kernel.org/lkml/20190115065832.23705-1-vigneshr) and I found the relevant fix in the code, though for some reason this fix does not work for N810.

The only way I managed to get wifi working was to reduce wait times for EOW and TXFFE bits in mcspi driver to 10ms and simply ignore any timeouts. Wifi comes up like this, and runs stably at around 50kbps speeds. Definitely not a long-term solution, but enough for further development.

I've started porting p54spi itself to OF probing - it works already and has been removed from the board init code, however it still uses module parameters for GPIOs, as well as probably some other hardcoded values which could all be moved to devicetree.

## Retu/Tahvo

Any reasonable new drivers for lcd backlight, framebuffer power control and general battery management will need additional patching of main retu-mfd driver, to allow mfd cells registering from dt-bindings. This has already been started for the tahvo-ledpwm driver, however it should probably be extended to allow full mfd_cell capabilities like id, reg address, resources and such. At the very least it should support irq resources as those are already used in existing cell drivers like retu-powerbtn, and will for sure be necessary for charging control.

# QEMU notes

I'm using qemu to ease basic development. It was really priceless to get rfbi and lcd control working, at least to memory/register level.
Of course it does not fully reflect all the hardware components like clocks, reset/power gpios and even less any interface timings between chips, so once the "display" came up in qemu, there was still some way to go to have it come up and get a reasonable refresh rate on actualy hardware, but generally it could be relied on, as long as something did not come up in qemu it would also not work on the device.

It also helps a lot to be able to inspect what the kernel is actually doing by recompiling qemu with additional debug output in specific emulated components

There are however several serious issues with emulation of n810 hardware in qemu, which make the kernel fail in places that actually work on real hardware:

 - MIPID emulation does not support 10-bit spi transfer. This was not an issue with older kernels, however modern ones seem to use it. I've taken the main mipid_transfer function from the similar driver for panel-sony-acx565akm. It works fine on real hardware but qemu crashes with equivalent of "not-implemented" message. As the original transfer function from 3.10 kernel no longer worked (probably because of changes in underlying spi drivers), I've put together a simple patch for qemu to allow 10bit messages. Actually it's just the first command byte being sent in 10bit mode, so it was not so hard, however it uncovered another issue which got overlooked with older kernels and n8x0 panel drivers. It seems the actual spi implementation in qemu is not fully functioning. For example reading the 3-byte ls041y ID works only the first time ; if you try to read it twice in series, the second read returns invalid data (actually returns 0:<id0>:<id1> instead of <id0>:<id1>:<id2>). I have confirmed the same kernel code works just fine on real hardware, so it must be qemu-related issue. I have a primitive working fix for this, by tweaking some flags in several mcspi control/status/config registers, although again no idea if it is actually correct. It makes MIPID work much better and does not seem to disturb anything else, so... I'll have to put it together and try sending it to qemu guys, ask what they think about it.

 - A lot more serious, and with no fix yet, is some kind of race condition ; probably between dma and mmc interrupts, which makes the kernel "freeze" after a short while as soon as it starts reading MMC more heavily ; aka. as soon as it starts booting userspace.
  - It is not fully deterministic, by including extensive debug into qemu mmc emulation I confirmed it happens in lots of different situations, regardless of mmc address, number of blocks being read or pretty much anything else. This happens with all kernel version I've tried, from the ancient 2.6.3x I dug up somewhere, through 3.10 and 4.19 to latest 5.15.
  - Sometimes the kernel manages to actually start the init process, especially if it's something small like a bash or even better statically built busybox shell. Any further activity in that shell usually hung up quite fast.
  - "hung-up tasks" detection in kernel reported the process stuck somewhere in ext4 fs code, waiting for block device transfer to finish. Some serious tracing in qemu sd/mmc code indicated in some situations a dma-finish interrupt arrived sooner than it should, without a corresponding mmc-finish. I'll have to redo the tests sometime; I had to scratch all the debug output, because it was making the behavior even more non-deterministic (hence my suspicion of a race condition).
  - For 3.10 kernel I was even able to mitigate this behaviour by adding a single debug output in a very specific place in omap mmc interrupt handler chain - it had to be a a real dmesg printout, simple sleep would not help for whatever strange reason ; neither did any deferred-work experiments I've tried. No idea what was going on there, but the same hack did not help for any newer kernel.
  - I've never experienced any such issues on real device, all the kernels booted up to userspace and worked fine, sometimes for hours, until I've shut it down or managed to crash it in some other unrelated way :-)

