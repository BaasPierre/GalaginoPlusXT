# GalaginoPlusXT
New games for galaginoplus
This port is NOT by the original authors, so please do not bother them with issues.
Also i do have custom controllers for my games

Used [GalaginoPlus - VirtualClaudioBoy](https://github.com/VirtualClaudioBoy/GalaginoPlus)

Games added here:

Pinball Action

Ghost n Goblins

Sound is not 100% emulated

Note: Ghosts'n Goblins is natively a horizontal/landscape game.

The Fairyland Story (flstory)

These builds is for landscape.

Rotation can be added.

Enjoy!

Ps, regards to edits in audio.cpp file you need to diff out to see the changes, but for other edits like machines.h files add example (<#ifdef ENABLE_PBACTION

#include "machines/pbaction/pbaction.h"

#endif>

and src\machines\machineBase.h add its new instance MCH_PBACTION or MCH_GNG