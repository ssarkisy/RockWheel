/*
 * This config file is for the Apple iPod Color/Photo
 */

#define IPOD_ARCH 1

#define MODEL_NAME   "Apple iPod Color/Photo"

/* For Rolo and boot loader */
#define MODEL_NUMBER 3

/* define this if you use an ATA controller */
#define CONFIG_STORAGE STORAGE_ATA

/*define this if the ATA controller and method of USB access support LBA48 */
#define HAVE_LBA48

/* define this if you have recording possibility */
#define HAVE_RECORDING

/* Define bitmask of input sources - recordable bitmask can be defined
   explicitly if different */
#define INPUT_SRC_CAPS (SRC_CAP_MIC | SRC_CAP_LINEIN | SRC_CAP_FMRADIO)

/* define the bitmask of hardware sample rates */
#define HW_SAMPR_CAPS   (SAMPR_CAP_44)

/* define the bitmask of recording sample rates */
#define REC_SAMPR_CAPS  (SAMPR_CAP_44)




/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this if you want album art for this target */
#define HAVE_ALBUMART

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

/* LCD dimensions */
#define LCD_WIDTH  220
#define LCD_HEIGHT 176
/* sqrt(220^2 + 176^2) / 2.0 = 140.9 */
#define LCD_DPI 141
#define LCD_DEPTH  16   /* 65536 colours */
/* The panel takes its pixels byte-swapped, but the framebuffer is RGB565 as
   on the iPod Video, as Finder's covers and the themes' pictures are:
   lcd_update_rect() swaps the bytes on the way out */
#define LCD_PIXELFORMAT RGB565

/* Define this if the LCD can shut down */
#define HAVE_LCD_SHUTDOWN

#ifndef BOOTLOADER
/* The panel of lcd_type 0 powered off while the iPod sleeps (HAVE_IDLE_SLEEP),
   and on again as it wakes, with the framebuffer's picture before the light
   (lcd-color_nano.c; backlight.c, powermgmt.c). Awake and dark it stays on,
   readable in daylight: the backlight's timeout doesn't turn it off, as it
   does the Video's BCM (LCD_SLEEP_TIMEOUT never). Types 1 to 3 stay on,
   dark */
#define HAVE_LCD_SLEEP
#define LCD_SLEEP_TIMEOUT (-1)
#define LCD_SLEEP_ASLEEP_ONLY
#endif

/* LCD stays visible without backlight - simulator hint */
#define HAVE_TRANSFLECTIVE_LCD

#define CONFIG_KEYPAD IPOD_4G_PAD

/* Define this to have CPU boosted while scrolling in the UI */
#define HAVE_GUI_BOOST

/* Define this to enable morse code input */
#define HAVE_MORSE_INPUT




/* define this if you have a real-time clock */
#define CONFIG_RTC RTC_PCF50605

/* No HAVE_RTC_ALARM, though the PMU can wake the iPod: an alarm's wake can't
   be told from the clock on every iPod, and Hold at that time starts Apple's
   firmware. As on the iPod Video */

/* Define this if you can switch on/off the accessory power supply */
#define HAVE_ACCESSORY_SUPPLY

/* Define this if you have a software controlled poweroff */
#define HAVE_SW_POWEROFF

#ifndef BOOTLOADER
/* Idle, a held Play and the Sleep Timer put the iPod to sleep, dark with all
   kept in memory, as on the iPod Video; it powers off after the hours Turn
   Off After Sleep sets, or sooner on a low battery (powermgmt.c). The WM8975
   stays on: there's no standby for it (HAVE_DAC_STANDBY) */
#define HAVE_IDLE_SLEEP
/* The sleep's current for Turn Off After Sleep's Auto, in microamps: the
   iPod Video's, measured asleep with its codec on at 24 MHz, until this
   iPod's own is measured */
#define SLEEP_CURRENT_UA 19300
#endif

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x80000

/* The number of bytes reserved for loadable plugins: Cover Flow keeps its
   covers in them while music plays, as on the iPod Video */
#define PLUGIN_BUFFER_SIZE 0x200000

/* Define this if you have the WM8975 audio codec */
#define HAVE_WM8975

#define AB_REPEAT_ENABLE
#define ACTION_WPSAB_SINGLE ACTION_WPS_BROWSE

/* define this if you have a disk storage, i.e. something
   that needs spinups and can cause skips when shaked */
#define HAVE_DISK_STORAGE

/* Define this for LCD backlight available */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS

/* Main LCD backlight brightness range and defaults */
#define MIN_BRIGHTNESS_SETTING      1
#define MAX_BRIGHTNESS_SETTING      31
#define DEFAULT_BRIGHTNESS_SETTING  20

/* define this if the unit uses a scrollwheel for navigation */
#define HAVE_SCROLLWHEEL
/* define to activate advanced wheel acceleration code */
#define HAVE_WHEEL_ACCELERATION
/* define from which rotation speed [degree/sec] on the acceleration starts */
#define WHEEL_ACCEL_START 360
/* define type of acceleration (1 = ^2, 2 = ^3, 3 = ^4) */
#define WHEEL_ACCELERATION 3

/* Define this if you can detect headphones */
#define HAVE_HEADPHONE_DETECTION

#define BATTERY_CAPACITY_DEFAULT 700 /* default battery capacity */
#define BATTERY_CAPACITY_MIN 700  /* min. capacity selectable */
#define BATTERY_CAPACITY_MAX 1200 /* max. capacity selectable */
#define BATTERY_CAPACITY_INC 10   /* capacity increment */


#define CONFIG_BATTERY_MEASURE VOLTAGE_MEASURE

/* Hardware controlled charging? */
#define CONFIG_CHARGING CHARGING_MONITOR

/* define this if the unit can be powered or charged via USB */
#define HAVE_USB_POWER

/* define this if the unit can have USB charging disabled by user -
 * if USB/MAIN power is discernable and hardware doesn't compel charging */
#define HAVE_USB_CHARGING_ENABLE
#define TARGET_USB_CHARGING_DEFAULT USB_CHARGING_FORCE

/* define current usage levels */
#define CURRENT_NORMAL     69  /* ~10h (700mAh), see FS#9072 */
#define CURRENT_BACKLIGHT  20  /* FIXME: this needs adjusting */
#if defined(HAVE_RECORDING)
#define CURRENT_RECORD     35  /* FIXME: this needs adjusting */
#endif

/* Define Apple remote tuner */
#define CONFIG_TUNER IPOD_REMOTE_TUNER
#define HAVE_RDS_CAP
#define CONFIG_RDS RDS_CFG_PUSH

/* Define this if you have a PortalPlayer PP5020 */
#define CONFIG_CPU PP5020

/* Define this if you want to use the PP5020 i2c interface */
#define CONFIG_I2C I2C_PP5020

/* We're able to shut off power to the HDD */
#define HAVE_ATA_POWER_OFF

/* Turn on SMART support */
#define HAVE_ATA_SMART

/* define this if the hardware can be powered off while charging */
//#define HAVE_POWEROFF_WHILE_CHARGING

/* The start address index for ROM builds */
#define ROM_START 0x00000000

/* The size of the flash ROM */
#define FLASH_SIZE 0x100000

/* Define this to the CPU frequency */
#define CPU_FREQ      11289600

#define CONFIG_LCD LCD_IPODCOLOR

/* USB On-the-go */
#define CONFIG_USBOTG USBOTG_ARC

/* enable these for the experimental usb stack */
#define HAVE_USBSTACK
#define USB_VENDOR_ID 0x05ac
#define USB_PRODUCT_ID 0x1204
#define HAVE_USB_HID_MOUSE

/* Rockbox's own USB mode answers as Apple's firmware does, with its names and
   an iPod photo's description, so that Finder sees an iPod photo and syncs
   it, as on the iPod Video. A build with USB_ALWAYS_APPLE_DISK_MODE defined
   restarts into Apple's disk mode instead, as before. */
/* #define USB_ALWAYS_APPLE_DISK_MODE */
#ifndef USB_ALWAYS_APPLE_DISK_MODE
#define USB_IPOD_IDENTITY

/* the part of a USB write taken at a time, and the blocks of one command
   written while the next comes in, as on the iPod Video */
#define USB_WRITE_BUFFER_SIZE (1024*32)
#define USB_WRITE_BEHIND
#endif

/* without a description, hand a computer over to Apple's disk mode instead */
#define USB_APPLE_DISK_MODE

/* start with a logo on black, the way the stock firmware starts, instead
   of the Rockbox logo: 12 px over the middle of the screen, which leaves
   room under it for Updating Library's bar and line */
#define STOCK_BOOT_LOGO
#define STOCK_BOOT_LOGO_Y 36

/* Define this if you have adjustable CPU frequency */
#define HAVE_ADJUSTABLE_CPU_FREQ

/* Define this if you can read an absolute wheel position */
#define HAVE_WHEEL_POSITION

#define HAVE_HARDWARE_CLICK

#define BOOTFILE_EXT "ipod"
#define BOOTFILE "rockbox." BOOTFILE_EXT

/* The bootloader mounts the disk and loads rockbox.ipod up to this many
   times, the drive reset or set up again and the disk mounted again in
   between: right after Apple's disk mode, a first read can fail
   (bootloader/ipod.c). No try starts later than BOOTLOADER_TRY_SECONDS
   after the bootloader did. */
#ifdef BOOTLOADER
#define BOOTLOADER_TRIES 5
#define BOOTLOADER_TRY_SECONDS 60
#endif

#define ICODE_ATTR_TREMOR_NOT_MDCT

#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_IAP_MULTIPORT
#define HAVE_SERIAL

/* DMA is used only for reading on PP502x because although reads are ~8x faster
 * writes appear to be ~25% slower.
 */
#ifndef BOOTLOADER
#define HAVE_ATA_DMA
#endif

/* Define this, if you can switch on/off the lineout */
#define HAVE_LINEOUT_POWEROFF

/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY
