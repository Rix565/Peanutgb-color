# Peanut-GB for NumWorks

This app is a [Game Boy](https://en.wikipedia.org/wiki/Game_Boy) emulator that runs on the [NumWorks calculator](https://www.numworks.com). It is based off of the [Peanut-GB](https://github.com/deltabeard/Peanut-GB) emulator.

Available on [Nwagyu](https://yaya-cout.github.io/Nwagyu/)

## Install the app

To install this app, you'll need to:
1. Download the latest `peanutgb.nwa` file from the [Releases](https://codeberg.org/Yaya-Cout/peanutgb/releases) page
2. Extract a `cartridge.gb` ROM dump from your GameBoy cartridge, or, alternatively, use the provided `src/flappyboy.gb` file.
3. Head to [my.numworks.com/apps](https://my.numworks.com/apps) to send the `nwa` file on your calculator along the `gb` file.

## How to use the app

The controls are pretty obvious because the GameBoy's gamepad looks a lot like the NumWorks' keyboard:

|Game Boy controls|NumWorks|
|-|-|
|Arrow|Arrows|
|A|Back|
|B|OK|
|Select|Shift|
|Select (Alternate, see below) |Home|
|Start|Backspace|
|Start (alternate)|Alpha|
|Start (alternate, see below)|OnOff|
|Toolbox|Write current save to storage|
|0|Write current save to storage and exit|

The following keys will change the behavior of the emulator:

|Key|Behavior|
|-|-|
|7|Show frame timings|
|9|Enable OnOff and Home keys and suspend the calculator|
|1|Use the original Game Boy color palette|
|2|Use a pure grayscale palette|
|3|Use an inverted grayscale palette|
|4|Use Peanut-GB original palette|
|+|Render on the entire screen|
|×|Render on the entire screen but keep ratio|
|-|Render at a 1:1 scale|

## About OnOff and Home keys

External apps can't use OnOff and Home keys for control as the kernel detect if
these keys are pressed and react by exiting the app (and suspending the
calculator for the OnOff key).

As an user, the only thing you probably want to know is that you need to press
the "9" key on the keyboard enable the OnOff and Home keys. It will suspend the
calculator, so you just need to press Onoff after that to restore your game.
You need to do this every time you enter the emulator.

It also have the nice side effect of allowing suspending the calculator without
exiting the emulator which is useful in a lot of situations. For example, is
you need to temporary suspend the calculator in a hurry as the teacher is
coming, all you need to do is pressing the "9" key (but you are not playing
during class, right? _~insert Anakin and Padme meme here~_)

For the technical details on what exactly happen when you press the 9 key, see
below:

There is no API to disable explicitly this unwanted behavior. To work around
this issue, we call the kernel method for enabling USB which, among other
things, disable the interrupts and handling of OnOff and Home keys. It also
shut down the keyboard (except the back key), so we need to power it back on.
Hopefully, when the calculator goes out of sleep, keyboard is reinitialized, but
not interrupts.

This is a bit hacky, but at least it's working and better than nothing. An
official implementation from NumWorks would be useful, but who know if they are
planning to implement API for external apps… There is no official API for using
the storage, external apps are using an implementation directly reading the RAM
the same way as computer does to read and write files in the calculator, so we
are far away from getting a method to disable OnOff and Home.

## Build the app

To build this sample app, you will need to install the [embedded ARM toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain) and [nwlink](https://www.npmjs.com/package/nwlink).

```shell
brew install numworks/tap/arm-none-eabi-gcc node # Or equivalent on your OS
npm install -g nwlink
make clean && make build
```
