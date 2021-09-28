#include <readline/readline.h>
#include <readline/history.h>

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) {
  siglongjmp(loop_env, sig);
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
    // Jeśli jest to token oznaczający przekierowanie wejścia
    if (token[i] == T_INPUT) {
      // Potencjalnie musimy zamnknąć deskryptor pliku
      MaybeClose(inputp);
      // Otwieramy plik do odczytu
      *inputp = Open(token[i + 1], O_RDONLY, 0);
      // Usuwamy token przekierowania
      token[i] = T_NULL;
      token[i + 1] = T_NULL;
      // Jeśli jest to pierwszy token przekierowania
      // to w n zapamiętujemy liczbę tokenów
      // występujących przed nim
      if (mode == NULL)
        n = i;
      // Zapisujemy informację o wykryciu przekierowania
      mode = T_INPUT;
      // Jeśli jest to token oznaczający przekierowanie wejścia
      // postępujemy analogicznie jak dla przekierowania wyjścia
    } else if (token[i] == T_OUTPUT) {
      MaybeClose(outputp);
      *outputp =
        Open(token[i + 1], O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
      token[i] = T_NULL;
      token[i + 1] = T_NULL;
      if (mode == NULL)
        n = i;
      mode = T_OUTPUT;
    }
    n++;
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
  pid_t pid = Fork();
  if (pid) { // Proces rodzica
    // Ustawiamy pgid utworzonego procesu
    Setpgid(pid, pid);
    // Zamykamy pliki potencjalnie otwarte przez do_redir
    MaybeClose(&input);
    MaybeClose(&output);
    // Tworzymy nowe zadanie i proces
    int job = addjob(pid, bg);
    addproc(job, pid, token);
    // Rozpoczynamy monitorować zadanie jeśli nie jest ono wykonywane w tle
    if (!bg) {
      exitcode = monitorjob(&mask);
    } else {
      msg("[%d] running '%s'\n", job, jobcmd(job));
    }
  } else { // Proces dziecka
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    // Ustawiamy pgid utworzonego procesu
    Setpgid(0, 0);
    // Możliwe, że trzeba przekierować wejście i wyjście
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      Close(input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      Close(output);
    }
    // Wykonujemy polecenie
    external_command(token);
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
  if (pid) { // Proces rodzica
    // Jeśli utworzony proces jest pierwszym w tym zadaniu,
    // to ustawiamy pgid na pid tego procesu
    if (pgid == 0)
      Setpgid(pid, pid);
    else
      Setpgid(pid, pgid);
    // Zamykamy pliki potencjalnie otwarte przez do_redir lub końce pipe
    MaybeClose(&input);
    MaybeClose(&output);
  } else { // Proces dziecka
    Sigprocmask(SIG_SETMASK, mask, NULL);
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    // Jeśli utworzony proces jest pierwszym w tym zadaniu,
    // to ustawiamy pgid na pid tego procesu
    if (pgid == 0)
      Setpgid(0, 0);
    else
      Setpgid(0, pgid);
    // Możliwe, że trzeba przekierować wejście i wyjście
    if (input != -1) {
      Dup2(input, STDIN_FILENO);
      Close(input);
    }
    if (output != -1) {
      Dup2(output, STDOUT_FILENO);
      Close(output);
    }
    // Wykonujemy polecenie
    int exitcode_for_builtin = builtin_command(token);
    if (exitcode_for_builtin >= 0)
      exit(exitcode_for_builtin);
    else
      external_command(token);
  }

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
  int token_offset = 0;
  // Iterujemy po kolejnych częściach pipeline'a
  for (int i = 0; i < ntokens; i++) {
    // Jeśli natrafiono na znak |, to
    if (token[i] == T_PIPE) {
      // znaleziono kolejne polecenie do uruchomienia
      pid = do_stage(pgid, &mask, input, output, token + token_offset,
                     i - token_offset);
      // Jeśli uruchomiliśmy pierwszy proces, to musimy utworzyć zadanie
      if (pgid == 0) {
        pgid = pid;
        job = addjob(pgid, bg);
      }
      addproc(job, pid, token + token_offset);
      // Przepinamy wejście dla kolejnego procesu
      input = next_input;
      mkpipe(&next_input, &output);
      // Przesuwamy się w pipline za ostatnio przetworzony fragment
      // (polecenie oraz znak |)
      token_offset = i + 1;
    }
  }
  // Zamykamy końce pipe, ponieważ został nam już tylko ostatni proces
  MaybeClose(&next_input);
  MaybeClose(&output);
  // Uruchamiamy ostatni proces
  pid = do_stage(pgid, &mask, input, output, token + token_offset,
                 ntokens - token_offset);
  addproc(job, pid, token + token_offset);
  // Rozpoczynamy monitorować zadanie jeśli nie jest ono wykonywane w tle
  if (!bg) {
    exitcode = monitorjob(&mask);
  } else {
    msg("[%d] running '%s'\n", job, jobcmd(job));
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(int argc, char *argv[]) {
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  Signal(SIGINT, sigint_handler);
  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  char *line;
  while (true) {
    if (!sigsetjmp(loop_env, 1)) {
      line = readline("# ");
    } else {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line)) {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
