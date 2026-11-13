# SimCoupe - A SAM Coupé Emulator

By Simon Owen (simon@simonowen.com)

## Introduction

SimCoupe emulates the SAM Coupé - a British Z80-based home computer released in
1989 by Miles Gordon Technology. See the Links section at the end of this
document for more information, including history and technical specifications.

---

## Loading Software

SAM starts up ready to accept BASIC programs, with software loaded from tape or
disk.

The steps required to boot a disk on a real SAM are:

   1) Press the reset button to Return to the start-up screen
   2) Insert disk in floppy drive 1
   3) Press F9 (or enter BOOT) to boot the disk

The equivalent steps in SimCoupe are:

   1) Press F12 to reset the emulated SAM
   2) Press F1 to browse for a disk image
   3) Press Numpad-9 or type BOOT and press Enter

The default SimCoupe settings actually make the final step unnecessary, as disks
inserted into drive 1 at the SAM start-up screen are booted automatically. To
boot a disk inserted at any other time, press F12 to reset then Numpad-9 to boot
(Numlock must be On).

Commercial titles will start automatically when booted, but some SAM disks found
online may not. Here are the common failure messages:

```
55 Missing Disk, 0:1
```
There's no disk in drive 1 - did you insert into drive 2 instead?

```
19 Loading error, 0:1
```
The boot sector (track 4, sector 1) could not be loaded. The disk could be
unformatted or may be damaged.

```
53 No DOS, 0:1
```
The disk does not have a valid boot sector, and cannot be booted. The default
SimCoupe settings avoid this error by substituting an internal DOS image, so
you're more likely to see the following error instead.

```
101 no AUTO* file, 0:1
```
DOS was booted, but no filename starting with "auto" was found to load. To avoid
this error, boot using BOOT 1 instead, which loads DOS but doesn't attempt to
auto-run any file. Try DIR 1 to list the files on drive 1, then LOAD n, where n
is a file number to load.

```
0 OK, 0:1
```
DOS was loaded and an "auto" file was found, but there was no auto-run line
number to execute from. Use LIST to check for a BASIC listing, and RUN to
execute it. Otherwise use DIR 1 for a directory listing, to manually select a
file to load.

---

## Disk Images

SimCoupe can use software in the following disk image types:

 `.MGT` - Simple sector dump of +D/SAM disks: 2 sides, 80 tracks/side, 10
 sectors/track, 512 bytes/sector = 819200 bytes. Older images in this format may
 have a .dsk file extension. This is the preferred format for normal format SAM
 disks.

 `.SAD` - SAm Disk format, created by Aley Keprt. Also a sector-based dump, but
 with a 22-byte file header allowing disk-wide geometry adjustments to
 sides/disk, tracks/side, sectors/track and bytes/sector. Normal SAM disks
 stored in SAD format are 819222 bytes, but a difference in track order prevents
 removing the 22-byte header to give an equivalent MGT image. Version 2 SAD
 images are the same basic format, but compressed using gzip.

 `.DSK` - Extended DSK (EDSK) images, originally used for Amstrad CPC and
 Spectum +3 disks. A flexible format able to represent all existing SAM disks.
 Images size is proportional to the disk geometry, with a normal SAM disk format
 around 840K.

 `.SBT` - Sam BooTable files, created by Andrew Collier. These are self-booting
 files designed to be copied to an empty SAM disk, then booted. While not
 technically disk images, SimCoupe treats them as such (read-only).

TeleDisk .TD0 (and other) images can be converted to EDSK using SAMdisk.

---

## Real Disks

If you are a Windows user and your PC motherboard still includes a floppy port
you can use many original SAM disks directly in SimCoupe. Even custom-formatted
titles such as Lemmings and Prince of Persia can be booted directly.

To use this you must connect your floppy drive directly to the motherboard using
a standard 34-pin cable. Very few modern motherboards still support this. USB
floppy drives will NOT work as they can't read the 10th sector from tracks.

You must also install a free 3rd party driver to give full access to the drive.
This is a one-time install and must be run by a user with Administrator access
rights. The driver installer is available from: https://simonowen.com/fdrawcmd/

To use real disks under Windows 7 or later:

   1) Install the fdrawcmd.sys driver, if not already installed
   2) Insert your SAM disk into PC drive A:
   3) Select "Open A:" from the File menu to use as SAM drive 1
   4) Boot/use the disk as normal

---

## Hard Disks

SimCoupe emulates the Atom and Atom Lite hard disk interfaces. Atom's DOS (BDOS)
is compatible with the original SAMDOS. The hard disk is treated as an array of
floppy-sized records, giving good compatibility with existing software.

Real hard disks and compact flash (CF) cards can be used under Windows, Linux
and Mac OS X. This gives a fast and reliable way to share data between with a
real SAM machine. For safety, only USB-connected disks can be opened in
SimCoupe.

If you don't have a real device to use, you can still work with existing hard
disk images files (HDF):

   1) Press F10 to open the SimCoupe options
   2) In the Disks section, click "..." next to the Atom interface type
   3) Browse to select an existing HDF image file to use
   4) Click OK twice and you're ready to boot BDOS

To create and prepare a new HDF image:

   1) Press F10 to open the SimCoupe options
   2) In the Disks section, click "..." next to the Atom interface type
   3) Enter a new path and size in megabytes (32MB = 40 floppy records)
   4) Click OK twice to create the disk and return to the emulation
   5) Boot a BDOS version (1.6e is recommended)
   6) With "Floppy drive" selected, press Cursor-right to view the files
   7) Select "Formatter" and press Enter
   8) Press Enter again, then "Y" to begin the Atom format
   9) Your HDF image is now ready to use!

For more details on using BDOS, visit Edwin Blink's pages:
  http://www.samcoupe-pro-dos.co.uk/edwin/software/bdos/bdos.htm

### Windows 7 or later

  1) Insert your CF card in the USB reader
  2) In SimCoupe, press F10 to open the options
  3) In the Disks section, click the combo-box down-arrow for the Atom
  4) Select the device name to use.
  5) Click OK twice and you're ready to boot BDOS

### Linux

The details for discovering the IDE/CF device path will depend on the Linux
distribution you're using, but is usually one of the following:

  - For IDE hard disks, try `fdisk -l` as root to list available devices.
  - For USB devices, try `tail -f /var/log/messages` then inserting it.
  - Failing those, browse through the raw `dmesg | less` output.

Once you've found the device path:

  1) In SimCoupe, press F10 to open the options
  2) In the Disks section, enter the device path for the Atom hard disk
  3) Click OK then Close, and you're ready to boot BDOS

### macOS

  1) Insert the CF card in your USB reader
  2) When warned about an unreadable disk, click Ignore
  3) Open Disk Utility and select your CF volume (orange USB icon)
  4) Click Info on the toolbar, and find the Disk Identifier (such as disk1)
  5) Prefix the identifier with `/dev/r` for the device path (`/dev/rdisk1`)
  6) In SimCoupe, press Cmd-F10 to open the options dialog
  7) In the Disks section, enter the device path for the Atom hard disk
  8) Click OK then Close, and you're ready to boot BDOS!

In most cases you only need to determine the device path once, as it will be the
same the next time you insert it. It will only change if other disk devices have
been added/removed.

---

## Printing

SimCoupe supports print-to-file on all platforms, making it easy to export code
listings to a plain text file. Configure as follows:

   1) Press F10 to open the options
   2) Select the Parallel section
   3) Under the Port 1 drop-down, select Printer
   4) In the Printer Device section, select "File: prntNNNN.txt"
   5) Click OK to accept the new settings.

You can now print from most applications, and use LLIST to print BASIC listings.
The output will be saved to a file in your Data Files directory, with a unique
name matching the template "prntNNNN.txt".

---

## Keyboard Input

The default SAM keyboard mode allows letters, digits and symbols to be typed as
normal on your keyboard, with SimCoupe automatically converting them to the
appropriate SAM key sequence. There's also a Spectrum mapping mode to use when
running Spectrum software, and a raw mode to disable the mappings.

The SAM has a keypad of function keys from F0 to F9 located on the right-side of
the keyboard. For similar key positions in SimCoupe, the SAM keypad is mapped to
the numeric keypad on your keyboard. You'll need to have Numlock enabled for
these keys to be recognised. Don't forget that when SAM software refers to
function keys, you must use the numeric keypad instead!

F1 to F12 keys are used for emulator functions, except when the debugger is
active. Under macOS some function keys may be used for system functions, and
require the Command key to be held in addition to the function key.

Holding the Windows key and pressing F1 to F10 will generate the corresponding
SAM function key F1 to F0. Recent versions of Windows process Win-F1 as help,
so Win-F11 can also be used to access the SAM F1 key.

```
             F1 = Open disk 1
       Shift-F1 = Eject disk 1
        Ctrl-F1 = Save disk 1
         Alt-F1 = New disk 1
             F2 = Open disk 2
       Shift-F2 = Eject disk 2
        Ctrl-F2 = Save disk 2
         Alt-F2 = New disk 2
             F3 = Tape browser
             F4 = Import data
       Shift-F4 = Export data
         Alt-F4 = Exit application
             F5 = Toggle TV aspect ratio
             F6 = Toggle display smoothing
             F8 = Toggle full-screen
             F9 = Debugger
       Shift-F9 = Save SAM screenshot in PNG format
            F10 = Options
            F11 = NMI Button
            F12 = Reset button
       Ctrl-F12 = Exit application
         Win-Fx = SAM function key Fx

      PrintScrn = Save SAM screenshot in PNG format
          Pause = Pause emulation
     Ctrl-Break = Reset
  Ctrl-Keypad * = Reset
  Ctrl-Keypad - = Normal emulation speed
       Keypad - = Reduce emulation speed
       Keypad + = Increase emulation speed
       Keypad * = Turbo speed
```

Turbo speed disables the frame sync and sound, and limits the display to just 5
frames per second. This usually gives a big speed boost, which is useful for
zooming through slow sections in games and demos, etc.


SAM shift modifier keys and special symbols are mapped as follows:
```
         Insert = Inv
      Left-Ctrl = Symbol
     Right-Ctrl = Cntrl
       Left-Alt = Cntrl (if enabled)
      Right-Alt = Edit  (if enabled)
       Menu Key = Edit
  ` (backtick)  = (c)
  . (on keypad) = (c)
    § (section) = #
```

The following additional combinations are also provided for convenience, since
they map common keys to the equivalent function on the SAM:

```
     Native key   SAM key
     ----------   -------
         Delete = Shift-Delete
        Numlock = Symbol-Edit  (toggles SAM BASIC keypad mode)
           Home = Cntrl-Left
            End = Cntrl-Right
        Page Up = F4
      Page Down = F1
```

---

## Debugger

The debugger provides code, text, data and graphical views of memory, with
typical debugging functions such as single-stepping and breakpoints.

The debugger starts in disassembly view, highlighting the next instruction.
Symbols are shown for ROM locations, and for custom code if built with pyz80
using the --mapfile= option. The register panel on the right shows the current
system state.

### Register panel

The first 7 lines of the panel show Z80 register values, with changed registers
display in pink text. Below it are the current interrupt mode, and the interrupt
state (EI or DI). The arrows below the SP value point to the top of the stack,
where the top 4 values are shown.

**IM** shows the current interrupt mode, and IFF1 status (DI/EI).

**Stat** shows the 5 interrupt status flags: O=midi-out, F=frame, I=midi-in,
M=mouse and L=line. These letters are visible when the corresponding interrupt
type is active (low) in the status port (F9/249).

**Scan** shows the line number (0-311) and line-cycle counter (0-383) for the
current raster position within the frame. If the position is within the visible
area, the raster is shown on the display as a pulsing white dot.

**T** shows the cycle offset within the current frame (0-119807). Below it is
the number of cycles since the debugger was last active. During single stepping
this shows the timing for the last instruction, including any I/O contention
delays. Stepping over an instruction will give the total time for the step.

**A/B/C/D** show the page present in each of the four 16K mamory banks. This
will be ROM 0-1, RAM 00-1F for internal memory, or EXT 00-FF for external
memory. Cyan text indicates the area is read-only.

**L/H/V/M** show the current LMPR/HMPR/VMPR paging register values, plus the
current display mode (1-4).

**Events** shows upcoming events, and the number of cycles before they are due:
```
            FINT = start of frame interrupt
            FEND = end of frame interrupt
            LINT = start of line interrupt
            LEND = end of line interrupt
            MIDI = MIDI OUT interrupt
            MEND = end of active MIDI OUT interrupt
            MTXF = end of TXFMST active in LMPR
            MOUS = mouse reset after strobe
            BLUE = Blue Alpha clock tick
            TAPE = next tape edge due
            ASIC = end of ASIC startup sequence
```

Keys active in all views:
```
               A = enter new view address
               B = breakpoint list
               C = code trace history
               D = disassembly view
               G = graphics view
               H = change HMPR page
               L = change LMPR page
               M = change screen mode
               N = number view
               T = text view
               V = change VMPR page
       F1 or KP0 = toggle ROM0
       F2 or KP1 = toggle ROM1
       F3 or KP2 = toggle RAM write-protection
       F4 or KP3 = toggle external RAM
          Ctrl-T = toggle debugger transparency
             Esc = exit debugger, or return to disassembly view
```

Disassembly View:
```
               S = toggle symbol display
               U = execute until condition is met
   F11/KP7/Space = single step 1 instruction
         F10/KP8 = step over instruction
      Ctrl-Space = step over instruction
 Ctrl-F11 or KP9 = step out of function
     Shift-Space = step out of function
        F5/F6/F7 = step 10/100/1000 instructions
     KP4/KP5/KP6 = step 10/100/1000 instructions
   Ctrl-F5/F6/F7 = step 10K/100K/1M instructions
    Ctrl-KP4/5/6 = step 10K/100K/1M instructions
    Ctrl-F10/KP8 = step over with code timing (no ints, border contention)
      Left/Right = scroll 1 byte
         Up/Down = scroll 1 instruction
       PgUp/PgDn = scroll 1 page
 Ctrl-Left/Right = move PC by 1 byte
    Ctrl-Up/Down = move PC by 1 instruction
          Return = debugger command mode (see below)
```

Text/Number View:
```
         Up/Down = scroll by 1 line
      Left/Right = scroll by 1 byte
       PgUp/PgDn = scroll by 1 page
```

Graphics View:
```
         1/2/3/4 = select screen mode
         Up/Down = scroll by 1 line
      Left/Right = scroll by 1 byte
    Ctrl-Up/Down = zoom in/out
 Ctrl-Left/Right = adjust column width by 1 byte
       PgUp/PgDn = scroll by 1 column
  Ctrl-PgUp/PgDn = scroll by 1 page
```

Trace View:
```
             Space = toggle single/double register display
                 S = toggle address symbol display
```

Debugger Command Mode:
```
           di / ei = disable/enable interrupts
              im M = set interrupt mode M (0-2)
             reset = reset emulation
               nmi = generate non-maskable interrupt
               zap = replace current instruction by NOP
         call ADDR = simulate call of address A
            push W = push 16-bit value W onto stack
         pop [reg] = pop 16-bit value, optionally to register
             break = set paging+interrupts in an attempt to return to BASIC
               x N = execute N instructions
    [x] until COND = execute until condition is true (one-shot)
          bpu COND = breakpoint on condition (permanent)
          bpx ADDR = execute breakpoint at ADDR with optional condition
 bpm ADDR [r|w|rw] = memory breakpoint with optional access specifier
 bpmr A B [r|w|rw] = memory range breakpoint from address A for length B
   bpio P [rw|r|w] = I/O breakpoint on port P
           bpint I = breakpoint on interrupt (frame/line/midi/midiin/midiout)
 flag +|- sz5h3vnc = set and/or reset flag bits
              bc N = clear breakpoint N (* for all)
              bd N = disable breakpoint N
              be N = enable breakpoint N
               exx = exchange BC/DE/HL with BC'/DE'/HL'
          ex de,hl = exchange DE with HL
            ld R,N = load register R with value N
             r R=N = load register R with value N
           out P,N = write value N to port P
poke A,N1[,N2,...] = poke address A with one or more values
```

Breakpoint addresses are resolved to a physical location, so they'll trigger if
the same underlying memory is access from a different paging position. They can
also be specified in page:offset format for an explicit location e.g.  bpx 1:0
will break when page 1 offset 0 (BASIC address 0x8000/32768) is executed.

Breakpoints accept an optional condition, by adding `if COND` to the end of the
command. If present, the breakpoint will only trigger when the expression is
true.

Single-stepping a HALT instruction will step into the interrupt handler,
assuming interrupts are enabled. Stepping over a HALT will completely execute
the handler, as if stepping over a call. Step-over also recognises JP/JR
instructions, and will single-step to follow the jump rather than attempting to
step over it.

To return to the current execution point after browsing other memory locations,
press A to enter a new address and enter `pc` as the expression. Alternatively,
single-step and the view will automatically return to the next instruction.

To aid to debugging, conditional instructions show whether or not the condition
is met by the current flags. If execution flow is changing, the current
highlight changes from yellow to green, and an arrow indicates whether the
change is above or below the current location.

Double-clicking on an instruction in disassembly view will set an execution
breakpoint for that address.

### Numeric expressions

 Operators:
 ```
   Unary:  + - ~ ! * =
   Binary arithmetic:  + - * / \ %
   Logical:  && || and or
   Comparison:  == != <> < > <= >=
   Bitwise arithmetic:  & | ^ band bor bxor
   Bitwise shift:  << >>
```

 ### Symbols
 ```
   Single registers: a f b c d e h l i r ixh ixl iyh iyl
   Double registers: af bc de hl af' bc' de' hl' ix iy sp pc
   Paging: lpage hpage vpage vmode lepage hepage rom0 rom1 wprot
   Registers: lepr hepr lpen hpen status lmpr hmpr vmpr midi border addr
   Interrupts: ei di iff1 iff2 im
   I/O: inval outval
   Display: dline sline
   Execution: inrom call autoexec
```

#### Functions
 ```
    PEEK <addr> = 8-bit lookup in currently paged RAM
   DPEEK <addr> = 16-bit lookup in currently paged RAM
```

The '=' unary operator has a special use in expressions. Its operand is
evaluated immediately, and the value inserted in the expression instead of the
operand itself. The first example below shows why this can be useful.

Example `UNTIL` expressions:

 - Break when the current value of HL changes: `hl != =hl`
 - Break at the next HALT instruction: `peek pc == 0x76`
 - Break when 123 is written to any port: `outval == 0n123`
 - Break when screen mode 3 is selected: `vmode == 3`
 - Break when hex 1234 is top of stack: `dpeek sp == 1234`
 - Break when the raster is drawing screen line 0: `sline == 0`
 - Break when A, B and IXl are equal: `(a == b) && (b == IXl)`

Execute Until breakpoints are only temporary, and cleared when the debugger is
next activated, regardless of whether they were triggered. This also applies to
other simple breakpoints, such as step-out and step-over.

The debugger works natively in hexadecimal, but allows values to be entered in
other bases using an appropriate prefix or suffix:
```
       Decimal:  0n12345
   Hexadecimal:  1234 or 0x1234 or 1234h or $1234 or &1234 or #1234
     Character:  "a" or 'a'
        Binary:  %10101100
```

Octal is not supported, so leading zeroes have no special meaning.

---

## Options

The section below describes all setting available in the Options (`F10`):

### System

_Internal RAM_ - the base SAM model comes with only 256kB main memory, with an
internal add-on board to boost it to 512kB. Many software titles require 512kB
to work correctly.

_External memory_ - external add-on packs are available to extend memory in 1MB
blocks. Programs needed to be written specially to use external memory, with
only a few titles doing so. They include MasterDOS, MasterBASIC and the TopGun
Demo.

_Custom 32K ROM image_ - if blank a built-in v3.0 ROM image is used.

_Use AL-BOOT ROM if Atom Lite is connected_ - if the Drive 2 is configured as an
Atom Lite device, this option applies ROM patches to automatically boot from it.
This is not available if a custom ROM image is in use.

### Display

_Bi-linear fitering (smoothing)_ - smooth the display image when stretching to
fill the SimCoupe window.

### Sound

_SID Interface_ - selects the type of SID chip connected to the Quazar SID
interface. The (default) 6581 is the traditional chip found in the original C64
machines, with a gritty sound. The 8580 is the chip found in newer C64 models,
with a cleaner and brighter sound.

_DAC on port 7C_ - select the type of DAC device present on SAM's port 7C. The
Blue Alpha sampler has a custom clock frequency for variable speed playback. The
SAMVoc and Paula (no relation to the Amiga chip!) are simple DAC output devices.

_MIDI Out_ - select a device for MIDI output, which may be a real device or
software synthesizer.

### Parallel

_Port 1/2_ - selects the device to connect to the virtual printer port. With
Printer selected you have a choice of printing to a file or a real printer
device. Mono-DAC emulates an 8-bit mono sound device, and EDdac/SAMdac an 8-bit
stereo device. The latter is highly recommended for use with Stefan's SAM MOD
Player.

_Printer device_ - if Printer is selected above, this is the file or device to
use for output. The "File:" option auto-generates a unique file to hold the
output, and saves it to your Data Files path.

_Automatically flush print jobs_ - if no data is sent to the port within 2
seconds, any remaining print data with be flushed to the output device.

### Input

_Keyboard_ - in SAM Coupe mode letters and symbols are converted to the key
sequence required to generate the same symbol on SAM. For example, pressing
Shift-0 on a typical PC keyboard generates ')'. SimCoupe converts it to Shift-9
to generate the same symbol on SAM. If you require a literal Shift-0, select
Disabled from the mode list. Automatic mode also detects the presence of a
Spectrum ROM and will use ZX Spectrum mappings.

_Use Left-Alt for Cntrl key_ - maps the Left-Alt key to the Cntrl key on the SAM
keyboard, in addition to the Right-Ctrl key. Left-Alt is located in a similar
keyboard position to SAM's Cntrl key. Note: enabling this option blocks normal
Windows menu combinations, such as Alt-F for the File menu. However, you can
still press and release Alt to activate the menu, then press F to open the file
menu and navigate as normal.

_Use Alt-Gr for Edit key_ - maps the Right-Alt key to the Edit key on the SAM
keyboard. Alt-Gr is located in a similar keyboard position to SAM's Edit key.

_Enable mouse interface_ - select to use your mouse with supported SAM software.
Clicking on the SimCoupe window will activate the mouse when a program is
reading it. For BASIC use, double-click the SimCoupe window to active it. To
release mouse control for normal desktop use, press Esc or switch to another
task using the keyboard.

### Joystick

_Player 1/2_ - selects up to 2 devices to use for control input. Each can be set
to control SAM joystick 1 (keys 6,7,8,9+0), SAM joystick 2 (keys 1,2,3,4+5), or
a Kempston joystick used by Spectrum software.

### Drive 1

_Device_ - select whether floppy drive 1 is installed in the left-hand bay. The
ROM only supports booting from drive 1, but later loading is supported from
either drive.

_Media_ - select either a disk image file or real device from the drop-down list
(if supported).

### Drive 2

_Device_ - select the type of device installed in in the right-hand bay. This
can be None, Floppy, Atom Classic or Atom Lite.

_Media_ - select either a disk image file or real device from the drop-down list
(if supported). Atom and Atom Lite devices support Master and Slave drives,
which can be either HDF images or real devices. Only pre-formated hard disks and
compact flash cards are listed, and you'll also need sufficient permission to
access the raw disk devices. Under Windows this requires the user be running as
an Administrator (elevated), or having SAMdiskHelper installed. Under Linux the
user will often need to be a member of the 'disk' group.

### Misc

_SAMBUS clock_ - the most common clock hardware interface, as used by MasterDOS
and BDOS.

_DALLAS clock_ - advanced clock hardware, supported by BDOS.

_Show disk drive activity lights_ - enables on-screen LEDs in the top left of
the display, showing when the drive is active. Floppy drives are shown in green,
Atom in orange, and Atom Lite in blue.

_Show status messages_ - enables the display of status text in the bottom right
of the display. This are used to confirm various user actions, such as ejecting
disks and changing runtime options.

_Show emulation speed_ - show the percentage of normal SAM running speed in the
upper-right of the display window.

_Ask before saving disk image changes_ - prompts for confirmation before saving
modifications back to disk images when they are ejected. This doesn't apply to
changes saved manually using Ctrl-F1/F2.

### Helpers

_Fast boot after hardware reset_ - accelerates the cold-boot process, avoiding a
few seconds delay while main memory is tested.

_Fast floppy disk access_ - accelerates the emulation speed when disks are being
accessed, to speed up loading and saving.

_Auto-load media inserted at startup screen_ - automatically load disks and
tapes inserted when SAM is showing the stripey boot screen.

_Allow booting from non-bootable disks_ - detects booting from an unbootable
disk and temporarily replaces the boot disk with either an internal DOS image or
a user-specified disk. Once DOS has booted the original disk is restored and the
boot process continued.

---

## Command-line Options

SimCoupe supports the following command-line options, which override setting in
the configuration file:
```
    -scale <int>            Windowed mode scaling: 1=50%, 2=100%, 3=150%
    -tvaspect <bool>        TV aspect ratio (default=yes)
    -mode3 <bool>           Sample odd pixels in low-res (default=no)
    -fullscreen <bool>      Start in full-screen mode (default=no)
    -depth <int>            Colour depth for full-screen (default=16)
    -visiblearea <int>      0=No Border, 1=Small Border, 2=TV Visible,
                            3=Full Active (default=2)
    -greyscale <bool>       Greyscale mode (default=no)
    -filter <bool>          Smooth emulated display (default=yes)
    -filtergui <bool>       Smooth built-in GUI display (default=no)

    -rom <path>             32K custom ROM image (blank for default v3.0)
    -romwrite <bool>        Enable memory writes to ROM (default=no)
    -albootrom <bool>       Enable Atom Lite boot ROM patches (default=no)
    -fastreset <bool>       Skip SAM power-on memory test (default=yes)
    -asicdelay <bool>       ASIC delay on first start (default=yes)
    -mainmemory <int>       Main memory size in kB: 256 or 512 (default)
    -externalmem <int>      External memory size in MB: 0 (default) to 4
    -cmosz80 <bool>         CMOS rather than NMOS Z80 (default=no)
    -speed <int>            Emulator speed percentage (default=100)

    -drive1 <int>           Drive 1: 0=none, 1=floppy
    -drive2 <int>           Drive 2: 0=none, 1=floppy, 2=Atom, 3=Atom Lite
    -turbodisk <bool>       Fast disk access sensitivity (default=yes)
    -dosboot <bool>         Automagically boot DOS (default=yes)
    -dosdisk <path>         Custom DOS boot disk (blank for SamDos 2.2)
    -stdfloppy <bool>       Assume real disks are normal format (default=yes)

    -disk1 <path>           Disk image file for drive 1
    -disk2 <path>           Disk image file for drive 2
    -atomdisk0 <path>       Atom hard disk image or device path (Master)
    -atomdisk1 <path>       Atom hard disk image or device path (Slave)
    -autoload <bool>        Auto-load media at startup screen (default=yes)

    -turbotape <bool>       Fast tape access (default=yes)
    -tapetraps <bool>       Use tape traps for instant loading (default=yes)

    -inpath <path>          Default path for input files
    -outpath <path>         Default path for output files
    -respath <path>         Default path for resource files

    -keymapping <int>       Keyboard mapping: 0=none, 1=auto, 2=SAM, 3=ZX
    -altforcntrl <bool>     Use Left-Alt for SAM Cntrl key (default=no)
    -altgrforedit <bool>    Use Alt-Gr for SAM Edit key (default=yes)
    -mouse <bool>           Mouse interface enabled (default=no)
    -mousemult <int>        Mouse sensitivity multiplier 5-200 (default=30)
    -mouseesc <bool>        Esc to release mouse capture (default=yes)
    -keyin <string>         Type text at startup (default=none)

    -joytype1 <int>         Joystick 1: 0=none, 1=Joy1, 2=Joy2, 3=Kempston
    -joytype2 <int>         Joystick 2: 0=none, 1=Joy1, 2=Joy2, 3=Kempston
    -joydev1 <string>       Joystick 1 device (default=none)
    -joydev2 <string>       Joystick 2 device (default=none)
    -deadzone1 <int>        Joystick 1 deadzone percentage (default=20)
    -deadzone2 <int>        Joystick 2 deadzone percentage (default=20)

    -parallel1 <int>        Parallel port 1 device: 0=none (default),
                             1=printer, 2=mono DAC, 3=stereo DAC
    -parallel2 <int>        Parallel port 2 device: 0=none (default),
                             1=printer, 2=mono DAC, 3=stereo DAC
    -printerdev <string>    Printer device name or path
    -printeronline <bool>   Printer online (default=yes)
    -flushdelay <int>       Printer flush delay in seconds (default=2)

    -midi <int>             0=none (default), 1=midi synth [Win32]
    -midiindev <string>     MIDI-in device name/path (future)
    -midioutdev <string>    MIDI-out device name/path

    -sambusclock <bool>     SAMBUS clock (default=yes)
    -dallasclock <bool>     DALLAS clock (default=no)

    -sound <bool>           Sound enabled (default=yes)
    -latency <int>          Sound latency: 1=best, 5=(default), 20=worst
    -dac7c <bool>           DAC on port 7C: 0=none, 1=Blue Alpha (default),
                            2=SAMVox, 3=Paula
    -samplerfreq <int>      Blue Alpha sampler frequency (defaut=18000)
    -sid <bool>             SID chip: 0=none, 1=6581 (default), 2=8580

    -drivelights <int>      Floppy drive LEDs: 0=none, 1=top, 2=bottom
    -profile <int>          Profiling stats: 0=off, 1=simple (default),
                             2=detailed percentage, 3=detailed timings
    -status <bool>          Show status messages (default=yes)

    -remote <bool>          Remote control on 127.0.0.1 (default=no)
    -remoteport <int>       Remote control TCP port (default=10001)
    -headless <bool>        No window, no audio, default settings; implies
                             -remote 1 (default=no)
    -background <bool>      Show the window without taking focus (default=no)

  Key:
    <bool>    0 or 1, true or false, yes or no
    <int>     an integer value in the range shown next to the parameter
    <string>  string of characters, in "quotes" if it contains spaces
    <path>    file/dir path, in "quotes" if it contains spaces
```

To restore the defaults settings, close SimCoupe and delete the file:
  - `%APPDATA%\SimCoupe\SimCoupe.cfg`  [Windows]
  - `~/.simcouperc`  [Linux]
  - `~/Library/Preferences/SimCoupe Preferences`  [Mac OS X]

## Remote Control

SimCoupe can be driven by scripts over a local TCP socket, using a text protocol
modelled on ZEsarUX's ZRCP: the same `command> ` prompt (`command@cpu-step> `
while the emulation is stopped) and, where they make sense, the same command
names and output formats. It only listens on 127.0.0.1.

```
    SimCoupe -remote 1                  window as usual, remote on port 10001
    SimCoupe -headless 1                no window or audio (for automated tests)
    SimCoupe -remote 1 -background 1    window shown without taking focus
    SimCoupe -headless 1 disk.mgt       boot a disk headless
```

Headless mode uses the default settings (your configuration file isn't read),
draws no on-screen status text, and never shows dialogs: messages go to stderr
and to the breakpoint log. `save-screen` still works, from the frame buffer.
With remote control or headless mode on, settings and the DALLAS clock state
are never saved.

Connect with any line-based TCP client (e.g. `nc 127.0.0.1 10001`), send one
command per line, and read up to the next prompt. Only one client is served at
a time; a new connection replaces the old one. `help` lists the commands and
`help <command>` describes one.

Numbers in command arguments are decimal unless written with an `H` suffix or a
`0x`, `$`, `#` or `&` prefix. Breakpoint conditions and the `printe`/`putv`
actions use the debugger's expression syntax (see "Numeric expressions"), where
bare numbers are hex.

A number must fit in a 32-bit signed integer; anything larger, or not a number
at all, is an error (`Error. Invalid number: ...`), as is a value outside the
range a command accepts (`Error. Value out of range ...`). Memory blocks
(`read-memory`, `write-memory*`, `load-binary`, `save-binary`) must lie inside
the current memory zone (64K for zone -1, which doesn't wrap at 0xFFFF) and be
no longer than 16M bytes, else `Error. Address or length out of range for this
memory zone`. A bad argument never changes any state or stops the emulator.
`set-register` takes 0-FFFFH for 16-bit registers, 0-255 for 8-bit registers and the
ports, 0-2 for IM and 0-1 for IFF1/IFF2, and applies nothing if any assignment on
the line is bad. `write-memory` values are -128 to 255, key codes 0-255, and `disassemble` takes
an address from 0 to FFFFH and up to 65536 lines. `run` and `run-frames` take
0 or more (`run-frames 0` just stops the emulation).

`send-keys-string ms text` types everything after the delay and one separating
space, so the text may be just spaces; with no text at all it is an error.

Exit status: SimCoupe returns 0 on a normal exit, 1 if it could not start, and
2 if remote control could not listen on its port (for example because another
copy already uses it). Scripts can use this to tell a port clash from other
failures.

Commands that need emulated time (`run-frames`, `cpu-step`, `run`,
`send-keys-string`, `send-keys-ascii`) reply when they finish; further input
waits until then. They keep running if the client disconnects.

`run` and `run-frames` can be interrupted, as `run` can in ZEsarUX: anything the
client sends while they are running (even a bare Return) stops the emulation
within a frame, at normal or turbo speed. The reply is
`Interrupted by client input at PC=xxxxH` (for `run-frames`, followed by
`after N of M frames`), like a breakpoint stop, and the emulation is left in
cpu-step mode. The input is then handled as usual, so `pause` or
`get-registers` both stop a run and answer. Unlike ZEsarUX, the input is not
thrown away. Commands sent together with the `run` (already received when it
started) don't interrupt it: they wait for it as before. A new connection's first input
stops a run left by a previous client too; that client gets no reply for it, as
it didn't send the run, but the stop is in the breakpoint log. A disconnect
alone doesn't stop a run.

With a window (`-remote 1` without `-headless`), only one side drives the
emulation at a time, the same way as SimCoupe's own pause. While remote control
has the emulation stopped (cpu-step mode), the debugger, the Options dialog and
the other dialogs don't open, and a key press in the window shows
"Stopped by remote control" on the status line instead; keys reach the SAM again
once the client resumes. While the window's debugger or a dialog is open, or the
emulator is paused, `run`, `run-frames` (also `run-frames 0`), `cpu-step`,
`send-keys-string` and `send-keys-ascii` reply
`Error. The emulator's debugger or a dialog is open in its window` (or
`Error. The emulator is paused (press a key in its window)`), and so does
`enter-cpu-step` while the debugger or a dialog is open. If the debugger or a
dialog opens, or the emulator is paused, while one of those commands is waiting,
it ends with `Interrupted by the emulator's debugger|dialog|pause at PC=xxxxH`
(for `run-frames`, followed by `after N of M frames`), also logged; the
emulation is then left running (not in cpu-step mode) once the window lets it
continue.

| Group | Commands |
|---|---|
| Session | `help`, `get-version`, `noop`, `quit`, `exit-emulator` |
| CPU | `get-registers` (ZRCP format plus LMPR/HMPR/VMPR), `set-register reg=value ...` (also LMPR, HMPR, VMPR, BORDER), `enter-cpu-step`/`pause`, `exit-cpu-step`/`resume`, `cpu-step`, `run [instructions]`, `reset-cpu`, `hard-reset-cpu` (also clears RAM), `generate-nmi`, `get-tstates`, `reset-tstates`, `disassemble [addr] [lines]` |
| Speed | `run-frames [N]` (run N frames, then stop; interruptible), `run-to addr [max-frames]` (run until PC reaches an address, reporting the T-states taken), `set-speed normal\|turbo\|percent` |
| Memory | `read-memory addr [len]`, `write-memory addr byte ...`, `write-memory-raw addr hex`, `load-binary file addr [len]`, `save-binary file addr len`, `get-memory-zones`, `set-memory-zone n` |
| Paging | `get-paging-state` (ports, mode, ROM/write-protect state and the page in each section) |
| Keys | `send-keys-event key 1\|0`, `send-keys-string ms text`, `send-keys-ascii ms code ...`, `release-keys` |
| Screen | `save-screen file.png`, `get-screen-mode` |
| Disks | `insert-disk 1\|2 file [boot]`, `eject-disk [1\|2]` |
| Breakpoints | `enable-breakpoints`, `disable-breakpoints`, `set-breakpoint`/`sb idx [cond]`, `set-breakpointaction`/`sba idx [action]`, `enable-breakpoint`/`eb idx`, `disable-breakpoint`/`db idx`, `get-breakpoints`, `set-membreakpoint addr|page:offset type [len]`, `clear-membreakpoints`, `set-portbreakpoint port type [mask]`, `clear-portbreakpoints`, `get-breakpoint-log`, `clear-breakpoint-log` |
| I/O log | `io-log enabled yes\|no`, `io-log filter [in\|out] [port[:mask] ...]`, `io-log get [count]`, `io-log clear` |
| History | `cpu-history enabled yes\|no`, `cpu-history clear`, `get-cpu-history [count]` |
| Coverage | `cpu-code-coverage enabled yes\|no`, `clear`, `get [symbols]`, `get-physical` |
| Symbols | `load-symbols file [append]`, `clear-symbols`, `get-symbols [prefix]` |

Memory zones: zone -1 (the default) is the CPU's current view of memory. Other
zones are SimCoupe page numbers (0-31 internal RAM, 32 upwards external RAM,
288/289 ROM0/ROM1); an address in a zone runs on into the following pages, so
zone 0 gives linear access to all of internal RAM.

Keys are injected into the keyboard matrix, so programs that scan the keyboard
hardware see them, not just the ROM. `send-keys-event` takes a SAM key name
(`shift`, `symbol`, `cntrl`, `return`, `space`, `escape`, `edit`, `inv`,
`up`/`down`/`left`/`right`, `f0`-`f9`, `a`-`z`, `minus`, `plus`, `equals`,
`quotes`, `semicolon`, `colon`, `comma`, `period`, `delete`, `tab`, `caps`) or
a decimal ASCII code (so digit keys are 48-57). Keys typed with
`send-keys-string` are held for `ms` milliseconds and released for the same
time, rounded to 20ms frames. Remote keys are ignored while the floppy drive is
running in turbo mode, as host keys are.

Breakpoints (1-100) fire when their condition becomes true, as in ZEsarUX, and
are only checked after `enable-breakpoints`. The default action stops the
emulation (cpu-step mode) and logs `Breakpoint fired: <condition>`. Other
actions log and carry on: `prints text`, `printe expr`, `printregs`,
`putv expr`, or any other non-waiting command. Action output goes to an
in-memory log read with `get-breakpoint-log`. Memory breakpoints (type 1 =
read, 2 = write, 3 = both, 0 = clear) stop the emulation. `set-membreakpoint
address type [length]` uses addresses in the CPU's view, so it also fires for
any other page mapped at that address. `set-membreakpoint page:offset type
[length]` watches a physical page wherever it is mapped (sections A to D, any
paging), and never fires for a different page at the same CPU address. The
location is written as `cpu-code-coverage get-physical` prints it: the page is
`RAMnn` (00 to 1F, or fewer with 256K), `EXTnn` (00 to 3F per MB fitted, up to
the configured external memory) or `ROM0`/`ROM1`, with `nn` in hex, and the
offset is hex (0 to 3FFF; an `H` suffix, `0x` or `$` is accepted), for example
`set-membreakpoint RAM0A:0123 2` or `set-membreakpoint EXT01:3F00 3 256`. The
length must stay inside the page. A page that does not exist in the current
configuration, an offset past the page, a bad type, and write types on ROM
(writes to ROM are discarded unless the `romwrite` option is on) are errors.
Ranges overlapping an existing one in the same kind (CPU or the same page)
replace it; `get-breakpoints` lists them (`Memory RAM0A:0123H-0123H: write`).
When one fires the waiting command replies `Memory breakpoint fired: write
RAM0A:0123H via 8123H, instruction at 4003H`: the page and offset, the CPU
address the page is mapped at when the check runs (the first section if it is
mapped twice, `(unmapped)` if the instruction paged it out) and the address of
the instruction that did the access (the emulation stops after it finishes, so
PC is past it). The CPU-view form reports the same way. Unlike ZEsarUX, whose
`set-membreakpoint` takes only a CPU address, the physical form is
SAM-specific.

Port I/O breakpoints and the I/O log. `set-portbreakpoint port type [mask]`
stops the emulation on an IN (type 1), an OUT (2) or either (3); type 0 removes
the breakpoint. Like the other breakpoints they are only checked after
`enable-breakpoints`, and `get-breakpoints` lists them (`Port 00FBH mask 00FFH:
out`). A port is a number (decimal, or hex with an `H` suffix) or a name:
`lmpr`, `hmpr`, `vmpr`, `border`, `keyboard`, `status`, `line`, `lpen`, `clut`,
`lepr`, `hepr`, `midi`, `attr`, `saa`, `clock`, `kempston`; a name is the low
byte of the port (`lmpr` = FAH), in any letter case. The SAM decodes most ports
from the low byte only, so a breakpoint matches when `(port & mask)` equals
`(breakpoint port & mask)`: the default mask is FFH for a port up to FFH (so
`OUT (C),A` with BC=1FFBH matches `hmpr`) and FFFFH above it. Give a mask to
match differently, for example `set-portbreakpoint 12FEH 2 FFFFH` for only that
full port. A breakpoint with the same port and mask is replaced; at most 100.
`clear-portbreakpoints` removes them all. When one fires the command that was
waiting replies `Port breakpoint fired: OUT 00FBH value 1FH, instruction at
EBBDH` and the emulation stops after that instruction finishes, so PC is
already past it; the address in the message is the IN or OUT itself (for a
repeating `INIR`/`OTIR` it is the instruction, once per transfer). The message
is also added to the breakpoint log. A breakpoint action can't be set.

`cpu-history` keeps the CPU's recent past, to see how it got to a breakpoint
stop. It is off by default, and nothing is recorded or costs anything until
`cpu-history enabled yes`; `enabled no` stops recording but keeps what was
recorded, and `cpu-history clear` empties it (and the dropped count). While it
is on, the state before every instruction that runs is added to a ring of 10000,
so it holds only what ran after it was turned on, and it survives `run`,
`run-frames` and stops. Every instruction that executes is recorded exactly
once, in order, including the first one after `run`, `run-frames`, `cpu-step` or
`exit-cpu-step` (also after `set-register pc=`). The instruction a breakpoint
stopped before has not run, so it is not in the history until you resume and it
runs. `get-cpu-history [count]` shows the newest `count`
(default 20, 1 to 10000), oldest first, under a header such as `CPU history on:
8 entries, 0 dropped, showing 8` (the dropped count is entries pushed out of the
full ring). Each line is `8003H RAM00:0003 3C       INC  A   AF=0000 BC=0300 ...`:
the CPU address of the instruction, the physical page and offset it was fetched
from, its bytes, its disassembly, and the registers (AF, BC, DE, HL, SP, IX, IY,
the alternate AF, BC, DE, HL, I, R, interrupt mode and flip-flops, and LMPR,
HMPR, VMPR) as they were before it ran, so after a breakpoint stop the last
line is the instruction before the one at PC. An instruction
repeated at the same address (a repeating `LDIR`, a `JR $` loop) is a single
line ending ` x8`, showing the registers before its last repeat. With symbols
loaded a line ends with `sym=label`, and the disassembly names jump targets as
for `disassemble`. An interrupt appears as the entry at its handler's address
(0038H in ROM0). While recording is on emulation is slightly slower, with no
difference measured at turbo speed in a 3000-frame run.

`io-log` records I/O without stopping: `io-log enabled yes|no` turns it on or
off, `io-log filter [in|out] [port[:mask] ...]` limits it to the given ports
(a name or number, optionally with a mask, in the same way as for breakpoints:
`io-log filter lmpr hmpr vmpr`) and to one direction (none given = every port,
both directions; a new `filter` replaces the old one, and a bad one changes
nothing), `io-log get [count]` shows the newest `count` entries (default all),
and `io-log clear` empties the log and the dropped count. `get` starts with a
line `I/O log on: 23 entries, 0 dropped, filter: ...` and then one line per
entry, oldest first: `frame=355 cycle=113246 pc=EBBDH out port=00FBH value=00H`.
`frame` counts frames run since SimCoupe started, `cycle` is the T-state in that
frame at the I/O, `pc` is the address of the IN or OUT instruction, and `port`
is the full 16-bit port the CPU put on the address bus (the high byte is
whatever the program had in A or B). At most 100000 entries are kept: when full
the oldest is dropped and counted, and the count shows in the header until the
log is cleared. The log records what the CPU did, including I/O the emulated
SAM ignores (for example to the ASIC ports while the ASIC is asleep). I/O done
by the remote itself (`set-register lmpr=...`) or by the debugger is not
logged. With the log off and no port breakpoints set, I/O costs nothing extra.
Unlike ZEsarUX, which has no port breakpoint command, the names and the mask
are SAM-specific.

Cycle counter. `get-tstates` replies with the position in the current frame
on its first line (as before), and `TOTAL=n` on a second: the T-states the CPU
has run since start-up, as frames run times 119808 plus the position in the
frame, so keeping it costs nothing per instruction. It stops and starts with
the emulation, and a CPU reset does not clear it. `reset-tstates` zeroes the
total (the position in the frame is a property of the emulation and stays).
Contention is included: the figures are the SAM's real timing, with the waits
for internal RAM and the ASIC ports, so they depend on the screen mode and
where in the frame the code runs. The total is also an expression variable,
`cycles`, for conditions, `printe` and `putv` while remote control is on (as
for any expression, a number without an `H` suffix or trailing dot is hex).
Expressions are 32-bit, so `cycles` is the low 32 bits of the total as a signed
number (it wraps after about six minutes of emulated time without
`reset-tstates`). Two logging breakpoints (`sb 1 PC=8007H`, `sba 1 putv
cycles`, and the same at the other address) therefore give the cycles between
two PCs by subtraction. `run-to addr [max-frames]` is the one-shot form: it
runs until PC reaches the address (a number or a label), stops, and replies
`Reached 800AH after 304 T-states` (counted from when the command was sent, up
to the point just before the instruction at the address runs). The instruction
at the current PC always runs first, so with PC already at the address it stops
the next time it arrives. After `max-frames` frames (default 1500; 0 for no
limit) it gives up with `Gave up after N frames without reaching 8000H,
PC=xxxxH, n T-states`, and the emulation is stopped. Like `run` it is ended
early by a breakpoint, by any input from a client, or by the window's debugger,
a dialog or its pause (and refused if one is open already), and can't be used
in a breakpoint action. Unlike ZEsarUX, whose `get-tstates` is only the
position in the frame and whose only reset command is `reset-tstates-partial`
for a separate partial counter, this is one running total with a plain
`reset-tstates`; ZEsarUX has no `run-to` command.

Symbols. `load-symbols file [append]` loads labels from an assembler's symbol
file: a pyz80 `--exportfile` (a Python pickle, as the Rebelstar II SAM build
writes), an sjasmplus `--sym=` file (`name: EQU 0x8000`), the label table at
the end of an sjasmplus `--lstlab` listing, or a plain `hexaddress=name` file;
the format is found from the contents. The file is read in full first: a bad
line gives `Error. Symbols not loaded, line 7: ...` and changes nothing (the
labels already loaded stay). It replaces the loaded labels, or adds to them
with `append` (a name loaded again, ignoring case, takes the new value). Values
above FFFFH (large constants) can't be addresses and are skipped, and counted in
the reply; small constants (`equ 7`) are loaded like any label, so they may
label low addresses. `get-symbols [prefix]` lists them (`8000 start`, sorted by
address, after a `Symbols: 3 of 12` line; the prefix ignores case) and
`clear-symbols` removes them. Labels are 16-bit values in the CPU's view, with no
page: sjasmplus gives the address in the section the code runs in, and pyz80
likewise. A label is accepted by name, in any letter case, anywhere an address
is (`read-memory`, `write-memory`, `write-memory-raw`, `load-binary`,
`save-binary`, `disassemble`, `set-membreakpoint`, and as a value in
`set-register`), also as `name+N` or `name-N`, and in breakpoint conditions,
`printe` and `putv` (`sb 1 PC=main_loop`); the name is looked up when the
command runs, so changing the symbols later doesn't alter a breakpoint already
set. A name that isn't loaded is an error (`Error. Unknown label: nosuch`), never
address 0; the one exception is inside a breakpoint condition, where a name made
only of hex digits (`beef`) that isn't a label is the number, as in the debugger.
With labels loaded they are shown, as `label` or `label+0x1A` (up to 7FH past the
nearest label before it): `disassemble` names jump and call targets (up to 3
past a label, as the debugger does) and puts a `label:` line before an
instruction at a label; `get-registers` and the other register replies end with
`SYMBOLS: PC:label+0x4 HL:other` (PC always, the others only when exactly at a
label); breakpoint, port, memory and interrupt stops add ` (label)` after the
instruction address; `io-log get` lines end with `sym=label` for the
instruction's address; and `cpu-code-coverage get symbols` writes
`8003(loop+0x2)` for each address (`get-physical` has no CPU address, so shows
none). With no labels loaded all output is as before and nothing is looked up.
Paging: a label is matched on its 16-bit address alone, whatever page is mapped
there when the output is made. Where several labels share an address one is shown
(the first by name); and the same address in two different pages (for example
the SAM drivers in page 0 and game code at 8000H, or ROM and RAM) can't be told
apart, so a label can name an address in a page it doesn't belong to, and one
set of labels should be loaded for the memory layout being debugged (load another
file to switch, or `append` two that don't overlap). While labels were loaded by
the remote, the window debugger's own symbol file loading leaves them alone.
Unlike ZEsarUX, which has no symbol commands (it only searches loaded source
code with `find-label`), these are SAM-specific additions.

Example session:
```
    send-keys-string 100 PRINT 2+3
    send-keys-ascii 100 13
    run-frames 10
    save-screen /tmp/screen.png
    enable-breakpoints
    sb 1 PC=8000H
    sba 1 printregs
    run-frames 50
    get-breakpoint-log
```

---

## Links

SimCoupe Homepage:
  https://simonowen.com/simcoupe

SimCoupe project page:
  https://github.com/simonowen/simcoupe
  
World of Sam archive:
  https://www.worldofsam.org

Wikipedia entry for the SAM Coupe (and for more links):
  https://wikipedia.org/wiki/SAM_Coupé

---

## Disclaimer

THIS PROGRAM AND DOCUMENTATION ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, NOT EVEN THE IMPLIED WARRANTY OF MERCHANTABILITY OR FITNESS FOR A
PARTICULAR PURPOSE. BY USING THE PROGRAM, YOU AGREE TO BEAR ALL RISKS AND
LIABILITIES ARISING FROM THE USE OF THE PROGRAM AND DOCUMENTATION AND THE
INFORMATION PROVIDED BY THE PROGRAM AND THE DOCUMENTATION.
