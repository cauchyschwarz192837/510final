// user/game.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "game.h"

#ifndef PROT_READ
#define PROT_READ   1
#define PROT_WRITE  2
#endif

#ifndef MAP_SHARED
#define MAP_SHARED  1
#endif

#define SHM_FILE "game.shm"
#define PADDLE_HALF 2   // paddle height = 2*PADDLE_HALF+1 = 5

static int shm_fd = -1;
static int shm_len = PGSIZE;

// --------------------------------------------------
// shared state setup using mmap(MAP_SHARED)
// --------------------------------------------------
static struct GameState*
open_shared_state(void)
{
  shm_fd = open(SHM_FILE, O_CREATE | O_RDWR);
  if (shm_fd < 0) {
    printf("game: open %s failed\n", SHM_FILE);
    return 0;
  }

  // Make sure the file is at least one page.
  char zero = 0;
  for (int i = 0; i < PGSIZE; i++) {
    if (write(shm_fd, &zero, 1) != 1) {
      // ignore errors; file may already be big enough
      break;
    }
  }

  shm_len = PGSIZE;
  struct GameState *st =
    (struct GameState *)mmap(0, shm_len, PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_fd, 0);
  if (st == (void *)-1 || st == 0) {
    printf("game: mmap failed\n");
    close(shm_fd);
    shm_fd = -1;
    return 0;
  }
  return st;
}

static void
close_shared_state(struct GameState *st)
{
  if (st)
    munmap((void*)st, shm_len);
  if (shm_fd >= 0)
    close(shm_fd);
}

// --------------------------------------------------
// drawing on console
// --------------------------------------------------

static void
draw_screen(struct GameState *st)
{
  // Clear screen and move cursor home (VT100 escape codes).
  printf("\x1b[2J\x1b[H");

  for (int y = 0; y < FIELD_HEIGHT; y++) {
    for (int x = 0; x < FIELD_WIDTH; x++) {
      char ch = ' ';

      // borders
      if (y == 0 || y == FIELD_HEIGHT - 1) {
        ch = '-';
      } else if (x == 0 || x == FIELD_WIDTH - 1) {
        ch = '|';
      }

      // paddles
      int p1x = 2;
      int p2x = FIELD_WIDTH - 3;
      if (x == p1x &&
          y >= st->paddle1_y - PADDLE_HALF &&
          y <= st->paddle1_y + PADDLE_HALF) {
        ch = '#';
      }
      if (x == p2x &&
          y >= st->paddle2_y - PADDLE_HALF &&
          y <= st->paddle2_y + PADDLE_HALF) {
        ch = '#';
      }

      // ball
      if (x == st->ball_x && y == st->ball_y) {
        ch = 'O';
      }

      printf("%c", ch);
    }
    printf("\n");
  }
  printf("Controls: W/S = left paddle, I/K = right paddle, q = quit\n");
}

// --------------------------------------------------
// physics process
// --------------------------------------------------

static void
physics_loop(struct GameState *st)
{
  while (st->running) {
    // consume input flags and move paddles
    if (st->p1_up && st->paddle1_y - PADDLE_HALF > 1) {
      st->paddle1_y--;
    }
    if (st->p1_down && st->paddle1_y + PADDLE_HALF < FIELD_HEIGHT - 2) {
      st->paddle1_y++;
    }
    if (st->p2_up && st->paddle2_y - PADDLE_HALF > 1) {
      st->paddle2_y--;
    }
    if (st->p2_down && st->paddle2_y + PADDLE_HALF < FIELD_HEIGHT - 2) {
      st->paddle2_y++;
    }

    // clear one-shot flags
    st->p1_up = st->p1_down = 0;
    st->p2_up = st->p2_down = 0;

    // move ball
    st->ball_x += st->ball_vx;
    st->ball_y += st->ball_vy;

    // bounce off top/bottom
    if (st->ball_y <= 1) {
      st->ball_y = 1;
      st->ball_vy = -st->ball_vy;
    } else if (st->ball_y >= FIELD_HEIGHT - 2) {
      st->ball_y = FIELD_HEIGHT - 2;
      st->ball_vy = -st->ball_vy;
    }

    int p1x = 2;
    int p2x = FIELD_WIDTH - 3;

    // left paddle collision
    if (st->ball_x == p1x + 1 &&
        st->ball_vx < 0 &&
        st->ball_y >= st->paddle1_y - PADDLE_HALF &&
        st->ball_y <= st->paddle1_y + PADDLE_HALF) {
      st->ball_x = p1x + 1;
      st->ball_vx = -st->ball_vx;
    }

    // right paddle collision
    if (st->ball_x == p2x - 1 &&
        st->ball_vx > 0 &&
        st->ball_y >= st->paddle2_y - PADDLE_HALF &&
        st->ball_y <= st->paddle2_y + PADDLE_HALF) {
      st->ball_x = p2x - 1;
      st->ball_vx = -st->ball_vx;
    }

    // ball out of bounds: reset to center
    if (st->ball_x <= 1 || st->ball_x >= FIELD_WIDTH - 2) {
      st->ball_x = FIELD_WIDTH / 2;
      st->ball_y = FIELD_HEIGHT / 2;
      st->ball_vx = (st->ball_vx >= 0) ? -1 : 1;
      st->ball_vy = (st->ball_vy >= 0) ? 1 : -1;
    }

    sleep(3); // control speed (3 ticks)
  }
}

// --------------------------------------------------
// input process (reads keyboard)
// --------------------------------------------------

static void
input_loop(struct GameState *st)
{
  while (st->running) {
    char c;
    int n = read(0, &c, 1);
    if (n < 1) {
      continue;
    }
    if (c == 'q') {
      st->running = 0;
      break;
    } else if (c == 'w' || c == 'W') {
      st->p1_up = 1;
    } else if (c == 's' || c == 'S') {
      st->p1_down = 1;
    } else if (c == 'i' || c == 'I') {
      st->p2_up = 1;
    } else if (c == 'k' || c == 'K') {
      st->p2_down = 1;
    }
  }
}

// --------------------------------------------------
// render process
// --------------------------------------------------

static void
render_loop(struct GameState *st)
{
  while (st->running) {
    draw_screen(st);
    sleep(2);
  }
}

// --------------------------------------------------
// main: set up shared state and fork three processes
// --------------------------------------------------

int
main(int argc, char *argv[])
{
  struct GameState *st = open_shared_state();
  if (st == 0) {
    exit(1);
  }

  // Initialize game state.
  st->running = 1;
  st->ball_x = FIELD_WIDTH / 2;
  st->ball_y = FIELD_HEIGHT / 2;
  st->ball_vx = 1;
  st->ball_vy = 1;
  st->paddle1_y = FIELD_HEIGHT / 2;
  st->paddle2_y = FIELD_HEIGHT / 2;
  st->p1_up = st->p1_down = 0;
  st->p2_up = st->p2_down = 0;

  int pid_render = fork();
  if (pid_render == 0) {
    // renderer
    render_loop(st);
    close_shared_state(st);
    exit(0);
  }

  int pid_input = fork();
  if (pid_input == 0) {
    // keyboard input
    input_loop(st);
    close_shared_state(st);
    exit(0);
  }

  // parent: physics
  physics_loop(st);

  // wait for children
  wait(0);
  wait(0);

  close_shared_state(st);
  exit(0);
}

