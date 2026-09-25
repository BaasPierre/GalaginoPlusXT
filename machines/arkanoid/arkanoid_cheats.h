#ifndef ARKANOID_CHEATS_H
#define ARKANOID_CHEATS_H

// ==========================================================================
// arkanoid_cheats.h  --  cheats from MAME's cheat database (cheat.7z,
// arkanoid.xml, www.mamecheat.co.uk).
//
// MAME runs a cheat's "run" script once a frame (cheat_manager::frame_update,
// on the frame notifier); arkanoid::apply_cheats() does the same at vblank.
// Set a cheat to 1 (or a value where noted) to enable it. ARKANOID_CHEATS 0
// removes them all (the harness builds that way so the MAME comparison stays
// exact). Cheats start once the game has passed its boot ROM/RAM check (a
// patched ROM byte or a RAM poke during it shows BAD HARDWARE).
// The one-shot MAME cheats ("... Now!", "Select Pill to Drop Now!") are not
// ported: they need a button press, and there is no menu to trigger them.
// Note: scores made with cheats on also go into the saved high score table.
// ==========================================================================

#ifndef ARKANOID_CHEATS
#define ARKANOID_CHEATS 0
#endif

// "Always Keep One Ball in Play":
//   if (C4A5 > E4) C4A5 = E2   -- the ball bounces back above the paddle line
#ifndef ARKANOID_CHEAT_KEEP_BALL
#define ARKANOID_CHEAT_KEEP_BALL 1
#endif

// "Don't die when ball is out":  EF62 = 00
#ifndef ARKANOID_CHEAT_DONT_DIE
#define ARKANOID_CHEAT_DONT_DIE 0
#endif

// "P1 Infinite Lives":  ED76 = 06
#ifndef ARKANOID_CHEAT_INFINITE_LIVES
#define ARKANOID_CHEAT_INFINITE_LIVES 0
#endif

// "P2 Infinite Lives":  ED7B = 06
#ifndef ARKANOID_CHEAT_INFINITE_LIVES_P2
#define ARKANOID_CHEAT_INFINITE_LIVES_P2 0
#endif

// "Infinite Credits":  C432 = 09
#ifndef ARKANOID_CHEAT_INFINITE_CREDITS
#define ARKANOID_CHEAT_INFINITE_CREDITS 0
#endif

// "Select Perm Ball Speed":  C462 = speed, 1 = slowest .. 14 = fastest
// (0 = off, the game's own speed)
#ifndef ARKANOID_CHEAT_BALL_SPEED
#define ARKANOID_CHEAT_BALL_SPEED 0
#endif

// Warp door (the exit on the right side):
//   1 = "Warp door always open"  C4CE = 01
//   2 = "Warp door never open"   C4CE = 00
//   0 = off
#ifndef ARKANOID_CHEAT_WARP
#define ARKANOID_CHEAT_WARP 0
#endif

// "Always 1 Hit for any brick": program patch 58C7 = 18 (jr nz -> jr),
// 5909 = C3 (jp c -> jp); the original bytes are put back while the boot
// check runs.
// Needs the program ROM in RAM (the boot line "ROM in RAM: 111" - page 1).
#ifndef ARKANOID_CHEAT_ONE_HIT_BRICKS
#define ARKANOID_CHEAT_ONE_HIT_BRICKS 0
#endif

// "Go to Last Level":  ED72 = 20 (round 33, DOH)
#ifndef ARKANOID_CHEAT_LAST_LEVEL
#define ARKANOID_CHEAT_LAST_LEVEL 0
#endif

// "Select Pill to Drop Every 5 Seconds":  every 300 frames C658 = pill
//   1 L (red) laser, 2 E (blue) enlarge, 3 C (green) catch, 4 S (orange) slow,
//   5 B (pink) warp door, 6 D (cyan) disruption/multiball, 7 P (grey) extra life
//   0 = off
#ifndef ARKANOID_CHEAT_PILL_EVERY_5S
#define ARKANOID_CHEAT_PILL_EVERY_5S 0
#endif

#endif
