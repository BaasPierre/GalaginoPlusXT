# GalaginoPlusXT

New games for galaginoplus.

This port is NOT by the original authors, so please do not bother them with issues.  
Also, I do have custom controllers for my games.

Recommended to use an ESP32-S3 because of memory restrictions.

Used [GalaginoPlus - VirtualClaudioBoy](https://github.com/VirtualClaudioBoy/GalaginoPlus)

---

## Games Added Here

### 🌅 Landscape Builds

#### Pinball Action

#### Ghosts 'n Goblins
* Sound is not 100% emulated.
* **Note:** Ghosts 'n Goblins is natively a horizontal/landscape game.

#### The Fairyland Story (flstory)

#### Bubble Bobble (boblbobl)
It uses the parent rom from `bublbubl`. You need to merge the roms with `boblbobl` into a single `boblbobl.zip` file.

#### Moonpatrol added (mpatrol) with cpu

---

### 📱 Portrait Builds

#### Arkanoid (arkanoid)
Contains its cheats as well, see `arkanoid_cheats.h`.

As this is a paddle game but the nostalgia remains, I did add this machine.  
Not using a rotary switch makes this a difficult game with a simple controller.  
The cheats will help. You can also see the file `arkanoid.h` to customize the feel for the paddle movement speed.

---

Rotation can be added.

Enjoy!

### 🛠️ Development Notes

* **audio.cpp**: Regard edits in the `audio.cpp` file, you need to run a diff to see the changes.
* **machines.h**: For edits like the `machines.h` files, add your example config:
  ```cpp
  #ifdef ENABLE_PBACTION
  #include "machines/pbaction/pbaction.h"
  #endif
  ```
* **src\machines\machineBase.h**: Add its new instance `MCH_PBACTION` or `MCH_GNG`.
