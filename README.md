# Clarion-C-Bus-CD-Changer-Emulator
Emulates a Clarion C-Bus CD Changer for the purposes of an aux input


<h1> What does this thing even do? </h1>
Small Arduino sketch built for an Arduino Nano or Uno that emulates the behaviour of a Clarion CDC634 CD Changer

When you select CD on your Clarion headunit, the Arduino responds with the expected commands to start audio playback from a CD Changer.

<h1> How do I make it work??</h1>
You need a couple of things

1x Arduino Uno or Nano <br>
1x 13 pin DIN Cable, I used a random ebay sourced cable that I believe was for an Atari ST peripheral? <br>
1x 3.5mm audio jack to wire to the DIN cable <br>

Pinout for Arduino Connection:

SCK:  Pin 2 DIN > Arduino Pin D2 <br>
SOI:  Pin 1 DIN > Arduino Pin D3 <br>
SRQ:  Pin 4 DIN > Arduino Pin D4 <br>
AC Enable:  Pin 11 DIN > Arduino Pin D5 <br>
Data Ground:  Pin 12 DIN > Arduino Ground <br>

Pinout for 3.5mm Jack <br>

Left Channel: Pin 7 DIN > Jack Left Channel <br>
Right Channel: Pin 8 DIN > Jack Right Channel <br>
Audio Ground: Pin 6 DIN > Jack Ground <br>

Once you're all wired, upload the clarion_aux_release.ino sketch to your Arduino Uno, connect em together and hit the CD Changer button on your headunit, you should see TRACK 1, and then a timer start counting up.

<h1> KNOWN ISSUES </h1>
If you start your headunit with the Arduino already powered, sometimes the display will get stuck at TRACK 1. Switch between the Radio/Tape and CD Changer on your head unit and it should start working.

<h2> Command Reference for people in the future, so they don't have to go through the hell I did trying to figure this out</h2>

Sniffing this out was frankly nightmarish, so here's a copy for anyone in the future that might need this info
| CMD | Meaning | Reply |
| -------- | -------- | -------- |
| F7 | Bus Reset | Echo only |
| FB | Alternate Reset | Echo only |
| 00 | Service poll, sent after SRQ and at power-up | F7 |
| 11 | Message poll | Send your next message |
| 09 | First command of select seq | 00 01 1A |
| 45, 50, 51, 56, 59 | Status queries, no idea what they do honestly. 56 might be mute? | 00 01 1A |
| 41 | Disc Magazine contents (how many discs) | 1A 55 55 54 (Disc 1 only loaded) |
| 4C | No idea, part of the startup routine | 00 00 |
| 3D, 42 | No idea, part of the startup routine, head gets mad if you echo it | 00 01 00 |
| 61 - 66 | Start disc 1 through 6 | |
| 70 | Release Audio | |
| 81 | Select track 1| |
| 5C | No idea, follows audio on | 00 01 06 |
| E1-E6 | Load disc 1-6, starts playback | 4X FF FF (X is the disc number 1 - 6 |
| 3E | Read table of contents | 00 01 07 |
| 5B | Play | 00 01 01 |
