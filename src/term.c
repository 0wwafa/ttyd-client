#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "utils.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t quit_requested = 0;
static volatile sig_atomic_t resize_requested = 0;
static int signal_pipe_write = -1;

static void signal_handler(int sig) {
  if (sig == SIGWINCH) {
    resize_requested = 1;
  } else {
    quit_requested = 1;
  }
  if (signal_pipe_write >= 0) {
    char c = (char)(sig == SIGWINCH ? 'w' : 'q');
    ssize_t ignored = write(signal_pipe_write, &c, 1);
    (void)ignored;
  }
}

bool term_init(tc_term *term, char *err, size_t err_len) {
  memset(term, 0, sizeof(*term));
  term->winch_pipe[0] = -1;
  term->winch_pipe[1] = -1;
  term->stdin_is_tty = isatty(STDIN_FILENO) == 1;
  term->stdout_is_tty = isatty(STDOUT_FILENO) == 1;
  term->title_enabled = term->stdout_is_tty;
  (void)err;
  (void)err_len;
  return true;
}

bool term_enter_raw(tc_term *term, char *err, size_t err_len) {
  if (term->raw_active) return true;
  if (!term->stdin_is_tty) return true; /* pipes pass data through untouched */

  if (tcgetattr(STDIN_FILENO, &term->saved_termios) != 0) {
    snprintf(err, err_len, "cannot read terminal attributes: %s", strerror(errno));
    return false;
  }

  struct termios raw = term->saved_termios;
  cfmakeraw(&raw);
  /* Keep the terminal responsive: the remote side handles flow control */
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    snprintf(err, err_len, "cannot set raw mode: %s", strerror(errno));
    return false;
  }

  term->saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  term->raw_active = true;
  return true;
}

void term_restore(tc_term *term) {
  if (term == NULL) return;
  if (term->raw_active) {
    tcsetattr(STDIN_FILENO, TCSANOW, &term->saved_termios);
    term->raw_active = false;
  }
}

void term_get_size(tc_term *term, int *columns, int *rows) {
  struct winsize ws;
  int fd = term->stdout_is_tty ? STDOUT_FILENO : (term->stdin_is_tty ? STDIN_FILENO : -1);
  if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    *columns = ws.ws_col;
    *rows = ws.ws_row;
    term->columns = *columns;
    term->rows = *rows;
    return;
  }

  *columns = term->columns > 0 ? term->columns : 80;
  *rows = term->rows > 0 ? term->rows : 24;

  /* Fall back to the environment, which helps when stdout is redirected */
  const char *env_cols = getenv("COLUMNS");
  const char *env_rows = getenv("LINES");
  long v = 0;
  if (env_cols != NULL && parse_long(env_cols, &v) && v > 0 && v < 10000) *columns = (int)v;
  if (env_rows != NULL && parse_long(env_rows, &v) && v > 0 && v < 10000) *rows = (int)v;
  term->columns = *columns;
  term->rows = *rows;
}

void term_set_title_enabled(tc_term *term, bool enabled) { term->title_enabled = enabled && term->stdout_is_tty; }

void term_set_title(tc_term *term, const char *title) {
  if (term == NULL || !term->title_enabled || title == NULL) return;
  size_t len = strlen(title);
  char *safe = xmalloc(len + 1);
  escape_for_terminal(title, len, safe);
  fprintf(stdout, "\x1b]0;%s\x07", safe);
  fflush(stdout);
  free(safe);
}

void term_install_signals(tc_term *term) {
  if (pipe(term->winch_pipe) != 0) {
    log_warn("cannot create the signal pipe: %s\n", strerror(errno));
    return;
  }
  for (int i = 0; i < 2; i++) {
    fcntl(term->winch_pipe[i], F_SETFL, fcntl(term->winch_pipe[i], F_GETFL, 0) | O_NONBLOCK);
    fcntl(term->winch_pipe[i], F_SETFD, FD_CLOEXEC);
  }
  signal_pipe_write = term->winch_pipe[1];

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;

  sigaction(SIGWINCH, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
#ifndef TC_IGNORE_SIGINT
  /* Ctrl-C is delivered to the remote shell in raw mode, only catch it when
   * the terminal is not in raw mode (eg: --readonly). */
  if (!term->raw_active) sigaction(SIGINT, &sa, NULL);
#endif
  signal(SIGPIPE, SIG_IGN);
  term->winch_installed = true;
}

int term_resize_fd(tc_term *term) { return term->winch_installed ? term->winch_pipe[0] : -1; }

void term_drain_resize(tc_term *term) {
  if (!term->winch_installed) return;
  char buf[64];
  while (read(term->winch_pipe[0], buf, sizeof(buf)) > 0) {
    /* just drain */
  }
}

bool term_take_resize(tc_term *term) {
  (void)term;
  if (resize_requested) {
    resize_requested = 0;
    return true;
  }
  return false;
}

bool term_should_quit(tc_term *term) {
  (void)term;
  return quit_requested != 0;
}

int term_read_stdin(tc_term *term, void *buf, size_t len) {
  (void)term;
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, len);
    if (n > 0) return (int)n;
    if (n == 0) return TC_STDIN_EOF;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return TC_STDIN_ERROR;
  }
}

size_t term_stdin_pending(tc_term *term) {
  (void)term;
  return 0;
}

#else /* _WIN32 */

#include <windows.h>

static volatile LONG quit_requested = 0;
static volatile LONG resize_requested = 0;

static BOOL WINAPI console_ctrl_handler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
      /* Ctrl-C is forwarded as input in raw mode, so reaching this point means
       * the user wants to leave (eg: --readonly). */
      InterlockedExchange(&quit_requested, 1);
      return TRUE;
    default:
      return FALSE;
  }
}

typedef struct {
  tc_term *term;
  HANDLE handle;
} reader_ctx;

static DWORD WINAPI stdin_reader(LPVOID param) {
  reader_ctx *ctx = (reader_ctx *)param;
  tc_term *term = ctx->term;
  char buf[4096];

  while (!term->reader_stop) {
    DWORD read = 0;
    if (!ReadFile(ctx->handle, buf, sizeof(buf), &read, NULL) || read == 0) {
      if (term->reader_stop) break;
      /* End of input: remember it by pushing a sentinel via queue_len == SIZE_MAX */
      WaitForSingleObject(term->queue_mutex, INFINITE);
      term->reader_stop = true;
      ReleaseMutex(term->queue_mutex);
      break;
    }
    WaitForSingleObject(term->queue_mutex, INFINITE);
    if (term->queue_len + read > term->queue_cap) {
      size_t cap = term->queue_cap ? term->queue_cap : 8192;
      while (cap < term->queue_len + read) cap *= 2;
      term->queue = xrealloc(term->queue, cap);
      term->queue_cap = cap;
    }
    memcpy(term->queue + term->queue_len, buf, read);
    term->queue_len += read;
    ReleaseMutex(term->queue_mutex);
  }
  return 0;
}

bool term_init(tc_term *term, char *err, size_t err_len) {
  memset(term, 0, sizeof(*term));
  term->stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
  term->stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  term->stdin_is_tty = GetConsoleMode((HANDLE)term->stdin_handle, (LPDWORD)&term->saved_in_mode) != 0;
  term->stdout_is_tty = GetConsoleMode((HANDLE)term->stdout_handle, (LPDWORD)&term->saved_out_mode) != 0;
  term->in_mode_saved = term->stdin_is_tty;
  term->out_mode_saved = term->stdout_is_tty;
  term->title_enabled = term->stdout_is_tty;
  (void)err;
  (void)err_len;
  return true;
}

bool term_enter_raw(tc_term *term, char *err, size_t err_len) {
  if (!term->stdin_is_tty) return true;

  /* Virtual terminal input gives us the same escape sequences as on *nix */
  DWORD in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
  if (!SetConsoleMode((HANDLE)term->stdin_handle, in_mode)) {
    /* Older Windows: fall back to a mode without echo/line editing */
    in_mode = 0;
    if (!SetConsoleMode((HANDLE)term->stdin_handle, in_mode)) {
      snprintf(err, err_len, "cannot set console input mode (error %lu)", (unsigned long)GetLastError());
      return false;
    }
  }
  term->raw_active = true;

  if (term->stdout_is_tty) {
    DWORD out_mode = (DWORD)term->saved_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode((HANDLE)term->stdout_handle, out_mode);
  }

  /* Console input is not select()able, so pump it from a thread */
  term->queue_mutex = CreateMutex(NULL, FALSE, NULL);
  reader_ctx *ctx = xcalloc(1, sizeof(reader_ctx));
  ctx->term = term;
  ctx->handle = (HANDLE)term->stdin_handle;
  term->reader_thread = CreateThread(NULL, 0, stdin_reader, ctx, 0, NULL);
  return true;
}

void term_restore(tc_term *term) {
  if (term == NULL) return;
  if (term->raw_active) {
    if (term->in_mode_saved) SetConsoleMode((HANDLE)term->stdin_handle, (DWORD)term->saved_in_mode);
    if (term->out_mode_saved) SetConsoleMode((HANDLE)term->stdout_handle, (DWORD)term->saved_out_mode);
    term->raw_active = false;
  }
}

void term_get_size(tc_term *term, int *columns, int *rows) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (term->stdout_is_tty && GetConsoleScreenBufferInfo((HANDLE)term->stdout_handle, &info)) {
    *columns = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (*columns > 0 && *rows > 0) {
      term->columns = *columns;
      term->rows = *rows;
      return;
    }
  }
  *columns = term->columns > 0 ? term->columns : 80;
  *rows = term->rows > 0 ? term->rows : 24;
  term->columns = *columns;
  term->rows = *rows;
}

void term_set_title_enabled(tc_term *term, bool enabled) { term->title_enabled = enabled && term->stdout_is_tty; }

void term_set_title(tc_term *term, const char *title) {
  if (term == NULL || !term->title_enabled || title == NULL) return;
  size_t len = strlen(title);
  char *safe = xmalloc(len + 1);
  escape_for_terminal(title, len, safe);
  fprintf(stdout, "\x1b]0;%s\x07", safe);
  fflush(stdout);
  SetConsoleTitleA(safe);
  free(safe);
}

void term_install_signals(tc_term *term) {
  (void)term;
  SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
  term->resize_poll_enabled = true;
}

int term_resize_fd(tc_term *term) {
  (void)term;
  return -1;
}

void term_drain_resize(tc_term *term) { (void)term; }

bool term_take_resize(tc_term *term) {
  (void)term;
  if (InterlockedExchange(&resize_requested, 0)) return true;
  return false;
}

bool term_should_quit(tc_term *term) {
  (void)term;
  return InterlockedCompareExchange(&quit_requested, 0, 0) != 0;
}

int term_read_stdin(tc_term *term, void *buf, size_t len) {
  if (term->queue_mutex == NULL) return 0;
  int result = 0;
  WaitForSingleObject(term->queue_mutex, INFINITE);
  if (term->queue_len > 0) {
    size_t n = term->queue_len < len ? term->queue_len : len;
    memcpy(buf, term->queue, n);
    memmove(term->queue, term->queue + n, term->queue_len - n);
    term->queue_len -= n;
    result = (int)n;
  } else if (term->reader_stop) {
    result = TC_STDIN_EOF;
  }
  ReleaseMutex(term->queue_mutex);
  return result;
}

size_t term_stdin_pending(tc_term *term) {
  size_t pending = 0;
  if (term->queue_mutex == NULL) return 0;
  WaitForSingleObject(term->queue_mutex, INFINITE);
  pending = term->queue_len;
  ReleaseMutex(term->queue_mutex);
  return pending;
}

#endif
