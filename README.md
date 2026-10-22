# Input Buffer

A [Geode](https://geode-sdk.org) mod for Geometry Dash 2.2081 that buffers early jumps, previews your trajectory, and can optionally avoid deadly clicks.

## Features

**Input buffer**
Tap jump slightly before landing and the jump fires on the exact physics tick you touch the ground. The window is counted in physics ticks, so lag and pausing don't skew it.
- Works for cube, ball, robot and spider
- Ignores taps meant for orbs
- Adjustable window (default 150 ms)

**Trajectory preview**
Two invisible ghost copies of your icon run through the game's own physics every frame:
- Green: your path if you click now
- Red: your path if you don't
- Yellow: the part of the red path where an early tap is caught by the buffer
- Red X: that path ends in death
- Blue dot: that path reaches an orb, pad or portal

Both players are shown in dual mode.

**Smart Click**
Uses the two predicted paths to help you:
- Your click is ignored if clicking dies and not clicking is safe
- It clicks for you if not clicking dies and clicking is safe
- If both die, or you're on an orb, you stay in control

Smart Click is an auto-play aid. Don't use it for verifying or rated runs.

## Settings

Geode button → Input Buffer → gear icon. Everything above can be toggled, and the buffer window, trajectory length and Smart Click lookahead are adjustable. Trajectory and Smart Click can each be limited to practice mode.

## Install

Download the `.geode` file from the Releases page (or from a CI run's **Build Output** artifact) and put it in `Geometry Dash/geode/mods`, then restart the game.

## Build

Requires the [Geode SDK and CLI](https://docs.geode-sdk.org/getting-started/).

```
geode build
```

Pushing to GitHub also builds the mod with the included Actions workflow.

## Compatibility

Targets Geode 5.11.2 and GD 2.2081 on Windows. Other platforms are untested.

## Known limitations

- Smart Click runs two simulations per physics tick, which can cost performance on weak machines. Lower the lookahead if you notice slowdown.
- Dual-mode input handling is based on observed game behavior and may need adjustment.
- Timing assumes GD's per-step `dt` is in 1/60 s units; timewarp triggers may skew the windows.