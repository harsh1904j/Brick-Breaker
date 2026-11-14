# Brick Breaker (C++ terminal game)

A terminal Brick Breaker with 100 levels, falling power-up tokens, difficulty
modes, save/resume, a high score table and a very rude commentator.
Single file, no external libraries. Works on Windows, Linux and macOS.

## Build and run

```
g++ -std=c++17 -O2 main.cpp -o brickbreaker
./brickbreaker          # Windows: .\brickbreaker.exe
```

Run it in a real terminal (the VS Code integrated terminal works well).

## Controls

| Key | Action |
|---|---|
| A / Left arrow | Move paddle left |
| D / Right arrow | Move paddle right |
| SPACE | Launch the ball |
| P | Pause |
| M | Sound on/off |
| Q | Quit (progress is saved) |

## Features

- **100 levels:** 10 brick patterns x 10 difficulty tiers, with boss levels
  every 10th level. Later tiers add multi-hit and unbreakable bricks, a faster
  ball and a narrower paddle.
- **Difficulty:** Easy (5 lives, slower ball), Normal (3 lives), Hard (2 lives,
  faster ball).
- **Tokens:** every 5th broken brick drops a token. Catch it with the paddle.

  | Token | Effect |
  |---|---|
  | `(M)` | Score x2 for 10 seconds |
  | `(B)` | Balls x5 |
  | `(R)` | Destroys 10 random breakable bricks |
  | `(L)` | Extra life |
  | `(W)` | Wider paddle for the level |
  | `<S>` | BAD: paddle shrinks |
  | `<F>` | BAD: balls move faster for 8 seconds |
  | `<X>` | BAD: left/right controls reversed for 8 seconds |

- **Save and resume:** progress is saved at the start of every level in
  `brickbreaker.sav` (next to the executable). On launch, choose Continue or
  New Game. Losing all lives restarts from Level 1.
- **High scores:** top 5 runs are kept in `brickbreaker.hs`, with level and
  difficulty.
- **Sound:** short beeps for hits, tokens and lost lives (Windows `Beep`;
  terminal bell elsewhere). Toggle with M.
- **Commentary:** the game roasts you using your name and age.

## How it works

- Cross-platform raw keyboard input (`conio.h` on Windows, `termios` elsewhere).
- ANSI escape codes for colors and flicker-free redraw (one buffered write per
  frame).
- Fixed ~38 FPS game loop with sub-stepped ball physics, so fast balls cannot
  tunnel through bricks.
- Levels are generated from pattern functions, so there is no level data file.
- Multi-ball is a `std::vector<Ball>`. Tokens are a second vector that is
  updated and caught each frame.
