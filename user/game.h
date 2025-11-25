#ifndef GAME_H
#define GAME_H

// Simple console Pong shared state for mmap-based game.
// All processes (physics, renderer, input) mmap the same file
// and share this struct in one page.

#define PGSIZE 4096

#define FIELD_WIDTH  40
#define FIELD_HEIGHT 20

// Shared game state; lives entirely inside one mmap page.
struct GameState {
  int running;      // 1 while game is running, 0 to quit

  int ball_x;
  int ball_y;
  int ball_vx;
  int ball_vy;

  int paddle1_y;    // center y of left paddle
  int paddle2_y;    // center y of right paddle

  // One-shot input flags, set by input process, consumed by physics.
  int p1_up;
  int p1_down;
  int p2_up;
  int p2_down;
};

#endif // GAME_H

