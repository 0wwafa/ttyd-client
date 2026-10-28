#ifndef TTYD_CLIENT_TERM_H
#define TTYD_CLIENT_TERM_H

#include <stdbool.h>
#include <stddef.h>

/* terminal helpers: raw mode, window size, title and signal handling */

#define TC_STDIN_EOF (-1)
#define TC_STDIN_ERROR (-2)

#ifndef _WIN32
#include <termios.h>
#endif

typedef struct {
  bool raw_active;
  bool stdin_is_tty;
  bool stdout_is_tty;
  bool title_enabled;
  int columns;
  int rows;

#ifndef _WIN32
  struct termios saved_termios;
  int saved_stdin_flags;
  int winch_pipe[2];
  bool winch_installed;
#else
  void *stdin_handle;
  void *stdout_handle;
  unsigned long saved_in_mode;  /* DWORD on Windows, unused elsewhere */
  unsigned long saved_out_mode;
  bool in_mode_saved;
  bool out_mode_saved;
  void *reader_thread; /* thread pumping console input into a queue */
  void *queue_mutex;
  char *queue;
  size_t queue_len;
  size_t queue_cap;
  bool reader_stop;
  bool resize_poll_enabled;
#endif
} tc_term;

/* Initialize the terminal state, without changing any mode */
bool term_init(tc_term *term, char *err, size_t err_len);

/* Put stdin in raw mode (no-op when stdin is not a tty) */
bool term_enter_raw(tc_term *term, char *err, size_t err_len);

/* Restore the original terminal state (safe to call multiple times) */
void term_restore(tc_term *term);

/* Query the terminal size, falling back to 80x24 */
void term_get_size(tc_term *term, int *columns, int *rows);

/* Enable/disable OSC 0 window title updates */
void term_set_title_enabled(tc_term *term, bool enabled);

/* Set the terminal window title (OSC 0), ignored when disabled or not a tty */
void term_set_title(tc_term *term, const char *title);

/* Install the signal handlers / console handler. Must be called before the
 * event loop, after term_enter_raw(). */
void term_install_signals(tc_term *term);

/* Return the file descriptor that becomes readable on SIGWINCH (POSIX only),
 * returns -1 on Windows and when no handler is installed. */
int term_resize_fd(tc_term *term);

/* Drain the resize wakeup pipe */
void term_drain_resize(tc_term *term);

/* Whether a resize happened since the last call */
bool term_take_resize(tc_term *term);

/* Whether the user asked to quit through a signal (SIGINT/SIGTERM/SIGHUP) */
bool term_should_quit(tc_term *term);

/* Read available stdin data. Returns the number of bytes read, 0 when nothing
 * is available, TC_STDIN_EOF on end of input or TC_STDIN_ERROR. */
int term_read_stdin(tc_term *term, void *buf, size_t len);

/* Number of buffered stdin bytes ready to be read (Windows queue) */
size_t term_stdin_pending(tc_term *term);

#endif  // TTYD_CLIENT_TERM_H
