#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
  // Szukamy dla jakich procesów nastąpiła zmiana stanu
  bool continue_update = false;
  do {
    continue_update = false;
    pid = waitpid(WAIT_ANY, &status, WUNTRACED | WNOHANG | WCONTINUED);
    // Jeśli Waitpid zaraportował status jakiegoś procesu
    if (pid > 0) {
      // Iterujemy po wszystkich zadaniach
      for (int j = 0; j < njobmax; j++) {
        // Iterujemy po wszystkich procesach zadania
        for (int p = 0; p < jobs[j].nproc; p++) {
          // Jeśli znaleźliśmy odpowiedni proces
          if (jobs[j].proc[p].pid == pid) {
            // Jeśli proces został wznowiony sygnałem SIGCONT
            if (WIFCONTINUED(status)) {
              jobs[j].proc[p].state = RUNNING;
              // Jeśli proces się zatrzymał
            } else if (WIFSTOPPED(status)) {
              jobs[j].proc[p].state = STOPPED;
              // Jeśli proces się zakończył
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
              jobs[j].proc[p].state = FINISHED;
              jobs[j].proc[p].exitcode = status;
            }
            // Waitpid zaraportował status znanego nam procesu,
            // więc trzeba kontynuować,
            // bo może być jeszcze jakiś status do zaraportowania.
            continue_update = true;
          }
        }
      }
    }
  } while (continue_update);
  // Sprawdzamy, czy nie trzeba zmienić stanu zadań
  bool all_process_state_same;
  int last_state;
  // Iterujemy po wszystkich zadaniach
  for (int j = 0; j < njobmax; j++) {
    // Jeśli dane zadanie nie jest pustym slotem
    if (jobs[j].pgid != 0) {
      all_process_state_same = true;
      // Iterujemy po wszystkich procesach zadania
      last_state = jobs[j].proc[0].state;
      for (int p = 1; p < jobs[j].nproc; p++) {
        // Jeśli stan procesu jest inny od poprzedniego to
        if (last_state != jobs[j].proc[p].state) {
          all_process_state_same = false;
          break;
        }
      }
      // Jeśli stan wszystkich procesów zadania jest taki sam, to
      if (all_process_state_same)
        // należy zmienić stan tego zadania
        jobs[j].state = last_state;
    }
  }
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
  if (state == FINISHED) {
    *statusp = exitcode(job);
    deljob(job);
  }

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  /* TODO: Continue stopped job. Possibly move job to foreground slot. */
  // Jeśli zadanie ma być przeniesione na pierwszy plan
  if (!bg) {
    // Przenosimy zadanie w miejsce zadania pierwszoplanowego w tablicy zadań
    movejob(j, FG);
    // Umieszczamy zadanie jako zadanie pierwszoplanowe dla terminala
    Tcsetpgrp(tty_fd, jobs[FG].pgid);
    // Ustawimay tryby (ang. modes) terminala zapisane dla danego zadania
    Tcsetattr(tty_fd, TCSADRAIN, &jobs[FG].tmodes);
    // Jeśli zadanie jest zatrzymane to
    if (jobs[FG].state == STOPPED)
      // wznawiamy je wysyłając sygnał SIGCONT do odpowiedniej grupy procesów
      Kill((-jobs[FG].pgid), SIGCONT);
    // Jeśli zadanie nie zmieniło jeszcze swojego stanu, to
    while (jobs[FG].state == STOPPED)
      // czekamy na dostarczenie sygnału
      Sigsuspend(mask);
    msg("[%d] continue '%s'\n", j, jobs[FG].command);
    // Zaczynamy monitorować zadanie
    monitorjob(mask);
  } else { // Jeśli zadanie ma pozostać w tle
    if (jobs[j].state == STOPPED)
      Kill((-jobs[j].pgid), SIGCONT);
    msg("[%d] continue '%s'\n", j, jobs[j].command);
  }

  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
  Kill((-jobs[j].pgid), SIGTERM);
  // Jeśli zadanie było zatrzymane,
  // to musimy wysłać mu SIGCONT, aby dostał on SIGTERM
  if (jobs[j].state == STOPPED)
    Kill((-jobs[j].pgid), SIGCONT);

  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

    /* TODO: Report job number, state, command and exit code or signal. */
    // Zapisujemy polecenie i odczytujemy stan zadania
    char *command = strdup(jobcmd(j));
    int status;
    int state = jobstate(j, &status);
    // Jeśli spełnia ono nasze kryteria
    if (state == which || which == ALL) {
      // to w zależności od stanu wyświetlamy odpowiedni komunikat
      if (state == RUNNING) {
        msg("[%d] running '%s'\n", j, command);
      } else if (state == STOPPED) {
        msg("[%d] suspended '%s'\n", j, command);
      } else if (state == FINISHED) {
        // w zależności od rodzaju zakończenia działania
        // wyświetlamy odpowiedni komunikat
        if (WIFEXITED(status)) {
          msg("[%d] exited '%s', status=%d\n", j, command, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
          msg("[%d] killed '%s' by signal %d\n", j, command, WTERMSIG(status));
        }
      }
    }
    free(command);
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
  // Jeśli jest taka potrzeba to przekazujemy zadaniu
  // kontrolę nad terminalem
  if (Tcgetpgrp(tty_fd) != jobs[FG].pgid) {
    Tcsetpgrp(tty_fd, jobs[FG].pgid);
    Tcsetattr(tty_fd, TCSADRAIN, &jobs[FG].tmodes);
  }
  // Czekamy aż zadanie przestanie się wykonywać
  int status;
  while ((state = jobstate(FG, &status)) == RUNNING)
    Sigsuspend(mask);
  // Jeśli zadanie się zatrzymało, to przenosimy je na drugi plan
  if (state == STOPPED) {
    Tcgetattr(tty_fd, &jobs[FG].tmodes);
    int new_job_num = allocjob();
    movejob(FG, new_job_num);
    msg("[%d] suspended '%s'\n", new_job_num, jobcmd(new_job_num));
    // Jeśli się zakończyło, to wyłuskujemy exitcode ze statusu
  } else if (state == FINISHED) {
    if (WIFEXITED(status)) {
      exitcode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      exitcode = WTERMSIG(status);
    }
  }
  // Oddajemy kontrolę nad terminalem powłoce (shellowi)
  Tcsetpgrp(tty_fd, getpgrp());
  Tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes);

  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  Signal(SIGCHLD, sigchld_handler);
  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
  // Iterujemy po wszystkich zadaniach
  for (int j = 0; j < njobmax; j++) {
    // Jeśli dane zadanie nie jest pustym slotem
    if (jobs[j].pgid != 0) {
      // Zabijamy zadanie
      if (killjob(j)) {
        // Jeśli zadanie nie było zakończone,
        // to czekamy aż się zakończy
        while (jobs[j].state != FINISHED) {
          Sigsuspend(&mask);
        }
      }
    }
  }

  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}
