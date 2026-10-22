# Input Buffer

## Input buffer
Tap jump a little **before** you land and the jump comes out on the exact physics tick you touch the ground.
- Cube, ball, robot, spider (ship/UFO/wave/swing already react mid-air)
- Ignores taps meant for orbs
- Adjustable window (default 150 ms)

## Trajectory preview
Two ghost copies of your icon run through the game's real physics every frame:
- **Green**: where you go if you click now
- **Red**: where you go if you don't
- **Yellow**: part of the red path where a tap will be caught by the buffer
- **Red X**: that path dies
- **Blue dot**: that path reaches an orb, pad or portal (the preview stops there)

Everything can be changed in the mod settings.

## Click Shifting
When you click and that click would kill you (and not clicking is safe), the click is held back and registers on the **first physics tick where it's safe**. If it never becomes safe within the Max Delay, it's dropped. In ship, wave and robot it does the same for releases.

It steps aside on orbs and while dashing, so those always react instantly.

**Auto Click** (off by default) also clicks for you when not clicking would kill you.

A small "Smart Click" label shows in the corner while either is on. These are assist features: don't use them for verifying or rated runs.
