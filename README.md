
Linux kernel for Nokia N810
===========================

This branch is based on linux-stable-rc 5.15.x, with additional changes for Nokia N810 internet tablet device (possibly N800 as well?).
Eventual goal is to get as much as possible patched into the upstream kernel.
Code quality of most changes is awful; main goal for now is to get the hardware working.


## Working

- LCD panel using old omap2fb system, no DRM yet
  - gpio1 and dispc require no-reset-on-init hwmod flag to the panel work properly (gpio1 because of LCD_RESET, no idea why dispc)
- LCD backlight with smooth PWM control (127 levels)
- touchscreen
- keyboard with backlight, lm8323 driver adjusted for dt-bindings
  - whole driver should be rewritten using matrix-keypad framework
- RGB LED in top left corner, working almost out-of-the-box with upstream lp5521 driver, just needed correct dt-bindings
- tsl2563 ALS sensor, providing illumination data over IIO
  - needs to be unbound from i2c device before suspending, else it fails in suspend() on i2c write timeout
- wifi
  - depends on nasty workaround for mcspi fifo dma problem, see [bellow](#wifi)
- USB
  - gadget subsystem works fine, ethernet connection via usb tested
  - host system **not** tested yet


## Not working

**Full suspend/resume**

Device can be put to sleep using 'echo mem >/sys/power/state', it also wakes up successfully if the wakeup event comes quick enough (before retu-watchdog kills everything). Both sliding the keyboard open as well as any standard keys connected to lm8323 chip work fine (after patching the driver to handle suspend/wakeup correctly). Power button does not seem to work as wakeup source (there is no support in the driver).
Big problem is the retu watchdog. If the device stays in sleep long enough for it to expire, it kicks in and turns everything off. This watchdog apparently cannot be disabled, it seems the only solution would be having retu-rtc wake the device up briefly at least every 50-60s, write the watchdog and go back to sleep (retu-rtc driver will need to support alarms first).
- perhaps some other timer-wakeup could be used? retu-rtc seems to be a barely working mess... **needs testing**
Waking up periodically just to do some background work will probably be necessary anyway, to handle stuff like wifi.

**Bluetooth**

N810 uses BC4-ROM chip, almost certainly CSR BlueCore using HCI-UART BCSP protocol. There is support for protocol itself in the kernel.
"hci_nokia" driver can be activated via dt-bindings including all gpios, however it fails no the protocol negotiation (most likely because it's not compatible with bcsp).
- there is a "Frame reassembly failed" error in hci_nokia.c::nokia_recv(), caused by hci_h4.c::h4_recv_buf(). There seems to be 60 bytes response coming from the device with packet type 0x00, which is not listed in hci_nokia.c::nokia_recv_pkts list.
New device driver will have to be written, combining OF probing and initialization from hci_nokia but using BCSP as uart protocol.


## Not tested

- Audio


## Work in progress

**Retu & Tahvo**

- battery management
  - retu-madc iio driver provides access to battery and charger voltage, BSI and battery temperature
    - Vbat and Vchg are converted to mV reasonably well according to manual measurements
    - BSI is converted to mOhm approximately using table from old openwrt 3.3 kernel source
    - temperature is just raw adc reading for now, inverse proportional (302 -> room temp ~23dC, 220 already warm to the touch, ~37-40dC??)
  - retu-regs used for now to allow user-space access to Retu status register -> check for battery and charger presence
  - same retu-regs used to read battery current from Tahvo, and control charging PWM output
  - very basic userspace charger written in bash works reasonably well
  - charging system is generally only working "reasonably well" ; even the original firmware produces very jumpy current flow from charger (checked with oscilloscope)
    - according to openwrt-linux-3.3, actual Ibat is 1/3 of the BATCURR reported by Tahvo ; preliminary observations confirm that. More precise tests pending until I set up external logging of Vbat/Ibat and Vchg/Ichg values independent of N810 itself. (interesting not-very-relevant todo: get Vchg measured at multiple points ; external / pcb connection of charge connector / same point as retu-madc)
    - if the battery is discharged enough, PWM goes to 100%, current consumption from charger is stable, Vchg and Vbat are pretty much the same as reported by retu-madc (Vchg =~ Vbat+80mV)
    - low battery can pull Vchg from 4.95V on the charger to as low as 3.8V on ADC while pulling >750mA from the charger -> ~ 1.15V x 0.75A = 0.86W wasted somewhere between the cable, connector and filter
    - it seems max. Ibat in CC mode does not exceed ~800mA. With blanked screen and cpu ~50% without any idling, even this full-current charging does not get the battery over ~40dC.
    - when Vbat climbs to ~4.1V the PWM goes lower, but it does not lower the current smoothly. Instead, it seems the current gets fully open in short bursts in periods of 1s and longer, depending on exact PWM register value. It almost looks like a PWM with a period of 1-1.2s, as ridiculous as that sounds. It would be nice to have someone else reproducing this behavior, my device DID survive several days flood, fully submerged in river water... It's pretty amazing it still works, doesn't mean everything works as it should.
  - Counterintuitive fun-fact: charging works a lot better with a low-power charger (like 5V/700mA) - that way Tahvo can run on 100% PWM longer because a part of voltage-drop happens outside. Also the CV charging is a bit better because Vbat spread is tighter ; battery can be charged to higher "minimal" voltage while still keeping the maximum under threshold.

- retu-rtc driver is still missing alarm implementation ; probably crucial for stable suspend/resume handling


## Next steps:

- *remove LCD panel driver dependency on direct Retu/Tahvo access*
  - **DONE:** Vtornado power for S1D13745 is controlled by vcore-tahvo-regulator and backlight uses leds-tahvo driver, both attached to the Tahvo MFD using dt-bindings

- try to upstream lm8323 driver patches for dt-bindings and standard matrix-keypad

- move RFBI and N8x0 panel driver to drm/gpu
  - trying to upstream RFBI **might** be a problem without other testable platforms, as well as practically no documentation for OMAP2420 itself...? :-)
  - 5.15 kernel is already quite big with at least basic modern features included. With basic debug symbols to produce readable stacktraces (quite necessary for development), it's currently ~10k under the 2MB limit imposed by NOLO. Not sure if drm won't push it over.

- find out correct init sequences for LCD panel and framebuffer so the no-reset hwmod flags can be removed from gpio1 and dispc modules (this is also necessary for proper standby mode most likely?)

- start adding retu/tahvo drivers for battery management
  - because of lack of documentation of both chips, best source will probably be latest reasonable [openwrt patches for v3.3 kernel](https://git.openwrt.org/?p=openwrt/openwrt.git;a=tree;f=target/linux/omap24xx/patches-3.3;hb=fa097e5ae5fddb82a077a0bb1676a512fa2d908e), as well as original (even more ancient) [vendor sources](http://repository.maemo.org/pool/maemo4.1.2/free/k/kernel-source-diablo/)
  - *first step will probably be some debug readout (sysfs?) of all registers from both retu and tahvo chips*
    - **DONE:** retu-regs driver exports both Retu and Tahvo registers into sysfs. I have already

- battery management
  - current monitoring could probably use another iio adc driver? (trivial single-channel with on/off control and interrupt trigger??)
  - does it make sense to create regulator driver for charging-pwm control? or put it directly into the power/supply/ driver?

- fix mcspi fifo dma issue for 16-bit spi transfers to have wifi working without the hack (no idea where to start without SoC docs)



######
# Hardware notes


## LCD

The display is composed of two parts:
- panel itself, a Sharp LS041Y3 connected to SPI1 using MIPID command protocol and LCD_RESET signal
- external framebuffer Epson S1D13745 connected to the RFBI interface (DSS DBI), using both LCD_RESET and POWERDOWN signals

The powerdown gpio seems to work fine (power consumption has to be investigated), pulling it down on display_disable still allows for successful wakeup.

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
- tahvo-ledpwm, vcore-tahvo-regulator and retu-madc are the first working prototypes, as well as retu-regs used for debug/test access



######
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

