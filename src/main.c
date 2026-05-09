#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

static struct termios orig_termios;

static void enable_raw_mode(void)
{
  tcgetattr(STDIN_FILENO, &orig_termios);

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ICANON | ECHO); // turn off canonical mode + echo
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw_mode(void)
{
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

#define INPUT_BUFFER_SIZE 4096
#define MAX_ARGS 256
#define MAX_JOBS 128
#define MAX_COMPLETIONS 128

typedef struct
{
  char *stdout_file;
  int stdout_append;
  char *stderr_file;
  int stderr_append;
} Redir;

/* ---- Builtin forward declarations ---- */
static int is_builtin(const char *cmd);
static void builtin_echo(char *args[], int argc, Redir *r);
static void builtin_type(char *args[], int argc, Redir *r);
static void builtin_pwd(Redir *r);
static void builtin_cd(char *args[], int argc);
static void builtin_jobs(void);
static void builtin_history(char *args[], int argc);
static void builtin_complete(char *args[], int argc);
static void builtin_declare(char *args[], int argc);
static void handle_tab_completion(char *line);

typedef struct
{
  int job_id;
  pid_t pid;
  char command[INPUT_BUFFER_SIZE];
  int running; /* 1 = running, 0 = done */
} Job;

typedef struct
{
  char command[128];
  char script[512];
} CompletionEntry;

typedef struct Var
{
  char *name;
  char *value;
  struct Var *next;
} Var;

static CompletionEntry completions[MAX_COMPLETIONS];
static int completion_count = 0;
static int last_completion_was_multi = 0;

static Job bg_jobs[MAX_JOBS];
static int bg_job_count = 0;

static char **history = NULL;
static size_t history_len = 0;
static size_t history_cap = 0;
static size_t history_saved_count = 0;
static ssize_t history_cursor = -1;

static Var *shell_vars = NULL;

static int cmp_strptr(const void *a, const void *b)
{
  const char *const *pa = a;
  const char *const *pb = b;
  return strcmp(*pa, *pb);
}

static int is_directory(const char *path)
{
  struct stat st;
  return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Insert `insert` at prefix_start replacing prefix_len characters, redraw prompt */
static int insert_at_prefix_and_redraw(char *line, size_t prefix_start, size_t prefix_len,
                                       const char *insert)
{
  size_t insert_len = strlen(insert);
  size_t line_len = strlen(line);
  size_t tail_len = (line_len >= prefix_start + prefix_len) ? (line_len - (prefix_start + prefix_len)) : 0;

  if (prefix_start + insert_len + tail_len + 1 > INPUT_BUFFER_SIZE)
    return -1; /* overflow */

  memmove(line + prefix_start + insert_len,
          line + prefix_start + prefix_len,
          tail_len + 1); /* include NUL */
  memcpy(line + prefix_start, insert, insert_len);

  /* redraw prompt cleanly */
  printf("\r\033[2K$ %s", line);
  fflush(stdout);
  return 0;
}

int read_input_line(char *buf, size_t size)
{
  size_t pos = 0;

  while (1)
  {
    int c = getchar();
    if (c == EOF)
      return -1;

    /* ENTER */
    if (c == '\n')
    {
      buf[pos] = '\0';
      putchar('\n');
      fflush(stdout);
      return 0;
    }

    /* ESC sequence */
    if (c == 0x1B)
    { /* ESC */
      int c1 = getchar();
      if (c1 == '[')
      {
        int c2 = getchar();

        /* UP ARROW */
        if (c2 == 'A')
        {
          if (history_len == 0)
            continue;

          if (history_cursor < 0)
            history_cursor = history_len - 1;
          else if (history_cursor > 0)
            history_cursor--;

          const char *h = history[history_cursor];

          /* Clear line completely before printing */
          printf("\r\033[2K$ %s", h);
          fflush(stdout);

          strncpy(buf, h, size);
          buf[size - 1] = '\0';
          pos = strlen(buf);
          continue;
        }
        if (c2 == 'B')
        {
          if (history_len == 0 || history_cursor < 0)
            continue;

          history_cursor++;

          if ((size_t)history_cursor >= history_len)
          {
            history_cursor = -1;
            printf("\r\033[2K$ ");
            fflush(stdout);
            pos = 0;
            buf[0] = '\0';
          }
          else
          {
            const char *h = history[history_cursor];
            printf("\r\033[2K$ %s", h);
            fflush(stdout);
            strncpy(buf, h, size);
            buf[size - 1] = '\0';
            pos = strlen(buf);
          }
          continue;
        }
        history_cursor = -1;
        /* ignore other arrows */
      }
      continue;
    }

    /* TAB → trigger completion */
    if (c == '\t')
    {
      buf[pos] = '\0'; // null‑terminate current line
      handle_tab_completion(buf);

      // After completion, redraw buffer and update pos
      pos = strlen(buf);
      continue;
    }

    // any other character resets multi‑completion state
    last_completion_was_multi = 0;

    /* normal character (do NOT echo) */
    if (pos + 1 < size)
    {
      buf[pos++] = c;
      putchar(c);
      fflush(stdout);
    }
  }
}

static int starts_with(const char *s, const char *prefix)
{
  if (prefix[0] == '\0')
  {
    return 1; // empty prefix matches everything
  }
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

/* safe file matches: ignore . and .., check opendir, bound matches */
static void find_file_matches(const char *dir, const char *prefix,
                              char *matches[], int *count)
{
  DIR *d = opendir(dir);
  struct dirent *ent;

  if (!d)
    return;

  while ((ent = readdir(d)) != NULL)
  {
    /* skip current/parent dir entries */
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;

    if (!starts_with(ent->d_name, prefix))
      continue;

    if (*count < MAX_COMPLETIONS) /* avoid overflow; choose appropriate limit */
    {
      matches[(*count)++] = strdup(ent->d_name);
    }
  }

  closedir(d);
}

/* Helper: compute longest common prefix length among matches */
static int compute_lcp(char *matches[], int count)
{
  if (count <= 0)
    return 0;
  int lcp = strlen(matches[0]);
  for (int i = 1; i < count; i++)
  {
    int j = 0;
    while (j < lcp && matches[0][j] && matches[i][j] && matches[0][j] == matches[i][j])
      j++;
    lcp = j;
    if (lcp == 0)
      break;
  }
  return lcp;
}

/* Helper: show matches horizontally separated by two spaces.
   scan_dir is the directory that was scanned to produce matches[] */
static void show_matches(char *matches[], int count, const char *line, const char *scan_dir)
{
  putchar('\n');

  for (int i = 0; i < count; i++)
  {
    if (i > 0)
      printf("  "); /* two spaces between entries */

    /* Determine whether to append '/' for display */
    char fullpath[1024];
    if (scan_dir && strcmp(scan_dir, ".") != 0)
      snprintf(fullpath, sizeof(fullpath), "%s/%s", scan_dir, matches[i]);
    else
      snprintf(fullpath, sizeof(fullpath), "%s", matches[i]);

    struct stat st;
    if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode))
      printf("%s/", matches[i]);
    else
      printf("%s", matches[i]);
  }
  putchar('\n');

  /* Reprint prompt + current line so user can continue typing */
  printf("\r\033[2K$ %s", line);
  fflush(stdout);
}

/* ---------- Utility ---------- */

static void add_history(const char *line)
{
  if (!line || !*line)
    return;
  if (history_len == history_cap)
  {
    size_t new_cap = history_cap ? history_cap * 2 : 64;
    char **new_hist = realloc(history, new_cap * sizeof(char *));
    if (!new_hist)
      return;
    history = new_hist;
    history_cap = new_cap;
  }
  history[history_len++] = strdup(line);
}

static void save_history_to_file(const char *path)
{
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (size_t i = 0; i < history_len; i++)
  {
    fprintf(f, "%s\n", history[i]);
  }
  fclose(f);
}

static void append_history_to_file(const char *path)
{
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  for (size_t i = history_saved_count; i < history_len; i++)
  {
    fprintf(f, "%s\n", history[i]);
  }
  fclose(f);
  history_saved_count = history_len;
}

static void load_history_from_file(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[INPUT_BUFFER_SIZE];
  while (fgets(line, sizeof(line), f))
  {
    line[strcspn(line, "\n")] = '\0';
    if (*line)
    {
      add_history(line);
    }
  }
  fclose(f);
  history_saved_count = history_len;
}

static void set_shell_var(const char *name, const char *value)
{
  Var *v = shell_vars;
  while (v)
  {
    if (strcmp(v->name, name) == 0)
    {
      free(v->value);
      v->value = strdup(value ? value : "");
      return;
    }
    v = v->next;
  }
  Var *nv = malloc(sizeof(Var));
  if (!nv)
    return;
  nv->name = strdup(name);
  nv->value = strdup(value ? value : "");
  nv->next = shell_vars;
  shell_vars = nv;
}

static const char *get_shell_var(const char *name)
{
  Var *v = shell_vars;
  while (v)
  {
    if (strcmp(v->name, name) == 0)
      return v->value;
    v = v->next;
  }
  return NULL;
}

/* Expand $VAR and ${VAR} using shell_vars and environment */
static char *expand_vars(const char *s)
{
  char *result = malloc(INPUT_BUFFER_SIZE);
  if (!result)
    return NULL;
  size_t ri = 0;
  size_t len = strlen(s);
  for (size_t i = 0; i < len && ri < INPUT_BUFFER_SIZE - 1;)
  {
    if (s[i] == '$')
    {
      if (i + 1 < len && s[i + 1] == '{')
      {
        i += 2;
        char name[256];
        size_t ni = 0;
        while (i < len && s[i] != '}' && ni < sizeof(name) - 1)
        {
          name[ni++] = s[i++];
        }
        name[ni] = '\0';
        if (i < len && s[i] == '}')
          i++;
        const char *val = get_shell_var(name);
        if (!val)
          val = getenv(name);
        if (val)
        {
          size_t vl = strlen(val);
          if (ri + vl >= INPUT_BUFFER_SIZE - 1)
            vl = INPUT_BUFFER_SIZE - 1 - ri;
          memcpy(result + ri, val, vl);
          ri += vl;
        }
      }
      else
      {
        size_t j = i + 1;
        char name[256];
        size_t ni = 0;
        while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_') && ni < sizeof(name) - 1)
        {
          name[ni++] = s[j++];
        }
        name[ni] = '\0';
        if (ni == 0)
        {
          result[ri++] = '$';
          i++;
        }
        else
        {
          const char *val = get_shell_var(name);
          if (!val)
            val = getenv(name);
          if (val)
          {
            size_t vl = strlen(val);
            if (ri + vl >= INPUT_BUFFER_SIZE - 1)
              vl = INPUT_BUFFER_SIZE - 1 - ri;
            memcpy(result + ri, val, vl);
            ri += vl;
          }
          i = j;
        }
      }
    }
    else
    {
      result[ri++] = s[i++];
    }
  }
  result[ri] = '\0';
  return result;
}

/* ---------- PATH lookup ---------- */

static char *find_in_path(const char *command)
{
  const char *path_var = getenv("PATH");
  if (!path_var)
    return NULL;
  char *paths = strdup(path_var);
  if (!paths)
    return NULL;
  char *saveptr = NULL;
  char *dir = strtok_r(paths, ":", &saveptr);
  struct stat st;
  while (dir)
  {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir, command);
    if (stat(full, &st) == 0 && (st.st_mode & 0111))
    {
      free(paths);
      return strdup(full);
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  free(paths);
  return NULL;
}

/* ---------- Completion registry ---------- */

static void register_completion(const char *command, const char *script)
{
  for (int i = 0; i < completion_count; i++)
  {
    if (strcmp(completions[i].command, command) == 0)
    {
      snprintf(completions[i].script, sizeof(completions[i].script), "%s", script);
      return;
    }
  }
  if (completion_count < MAX_COMPLETIONS)
  {
    snprintf(completions[completion_count].command,
             sizeof(completions[completion_count].command), "%s", command);
    snprintf(completions[completion_count].script,
             sizeof(completions[completion_count].script), "%s", script);
    completion_count++;
  }
}

static const char *lookup_completion(const char *command)
{
  for (int i = 0; i < completion_count; i++)
  {
    if (strcmp(completions[i].command, command) == 0)
      return completions[i].script;
  }
  return NULL;
}

static int run_completer_script(const char *script,
                                const char *cmd_name,
                                const char *current_word,
                                const char *prev_word,
                                const char *comp_line,
                                size_t comp_point,
                                char *output,
                                size_t out_size)
{
  if (!script || out_size == 0)
  {
    if (out_size > 0)
      output[0] = '\0';
    return -1;
  }

  /* Set COMP_LINE and COMP_POINT */
  char comp_point_str[32];
  snprintf(comp_point_str, sizeof(comp_point_str), "%zu", comp_point);
  setenv("COMP_LINE", comp_line ? comp_line : "", 1);
  setenv("COMP_POINT", comp_point_str, 1);

  int pipefd[2];
  if (pipe(pipefd) < 0)
  {
    if (out_size > 0)
      output[0] = '\0';
    return -1;
  }

  pid_t cpid = fork();
  if (cpid < 0)
  {
    close(pipefd[0]);
    close(pipefd[1]);
    if (out_size > 0)
      output[0] = '\0';
    return -1;
  }

  if (cpid == 0)
  {
    /* Child */
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    /* Try direct exec */
    char *exec_argv[] = {
        (char *)script,
        (char *)(cmd_name ? cmd_name : ""),
        (char *)(current_word ? current_word : ""),
        (char *)(prev_word ? prev_word : ""),
        NULL};

    execv(script, exec_argv);

    /* Fallback: run script with sh */
    char *sh_argv[] = {
        "sh",
        (char *)script,
        (char *)(cmd_name ? cmd_name : ""),
        (char *)(current_word ? current_word : ""),
        (char *)(prev_word ? prev_word : ""),
        NULL};

    execvp("sh", sh_argv);

    /* If both execs fail, print diagnostic */
    dprintf(STDOUT_FILENO, "EXECV_FAILED: errno=%d (%s)\n", errno, strerror(errno));
    _exit(127);
  }

  /* Parent */
  close(pipefd[1]);

  ssize_t total = 0;
  ssize_t n;
  char *p = output;
  size_t remaining = out_size > 0 ? out_size - 1 : 0;

  while (remaining > 0)
  {
    n = read(pipefd[0], p, remaining);
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0)
      break;
    total += n;
    p += n;
    remaining -= n;
  }

  if (out_size > 0)
    output[(total < (ssize_t)(out_size - 1)) ? total : (ssize_t)(out_size - 1)] = '\0';

  close(pipefd[0]);

  int status = 0;
  waitpid(cpid, &status, 0);

  if (total <= 0)
  {
    if (out_size > 0)
      output[0] = '\0';
    return -1;
  }

  return 0;
}

/* ---------- Jobs ---------- */

static void reap_jobs(void)
{
  for (int i = 0; i < bg_job_count; i++)
  {
    if (bg_jobs[i].running)
    {
      int status;
      pid_t r = waitpid(bg_jobs[i].pid, &status, WNOHANG);
      if (r == bg_jobs[i].pid)
      {
        bg_jobs[i].running = 0;
      }
    }
  }
}

static int next_job_id(void)
{
  int id = 1;
  for (;;)
  {
    int used = 0;
    for (int i = 0; i < bg_job_count; i++)
    {
      if (bg_jobs[i].job_id == id)
      {
        used = 1;
        break;
      }
    }
    if (!used)
      return id;
    id++;
  }
}

static void add_job(pid_t pid, const char *cmdline)
{
  if (bg_job_count >= MAX_JOBS)
    return;
  int job_id = next_job_id();
  bg_jobs[bg_job_count].job_id = job_id;
  bg_jobs[bg_job_count].pid = pid;
  bg_jobs[bg_job_count].running = 1;
  snprintf(bg_jobs[bg_job_count].command,
           sizeof(bg_jobs[bg_job_count].command), "%s", cmdline);
  printf("[%d] %d\n", job_id, pid);
  bg_job_count++;
}

static void print_jobs(void)
{
  reap_jobs();
  int total = bg_job_count;
  for (int i = 0; i < bg_job_count; i++)
  {
    const char *marker = " ";
    if (i + 1 == total)
      marker = "+";
    else if (total >= 2 && i + 1 == total - 1)
      marker = "-";
    if (bg_jobs[i].running)
    {
      printf("[%d]%s  Running                 %s &\n",
             bg_jobs[i].job_id, marker, bg_jobs[i].command);
    }
    else
    {
      printf("[%d]%s  Done                    %s\n",
             bg_jobs[i].job_id, marker, bg_jobs[i].command);
    }
  }
  /* remove done jobs from list */
  for (int i = bg_job_count - 1; i >= 0; i--)
  {
    if (!bg_jobs[i].running)
    {
      if (i != bg_job_count - 1)
      {
        bg_jobs[i] = bg_jobs[bg_job_count - 1];
      }
      bg_job_count--;
    }
  }
}

/* ---------- Parsing ---------- */

int is_space(unsigned char c)
{
  return c == ' ' || c == '\t';
}

/* parse_args with quotes and backslashes similar to Rust version */
static int parse_args(const char *input, char *args[], int max_args)
{
  int argc = 0;
  char *buf = strdup(input);
  if (!buf)
    return 0;
  char *dst = buf;
  char *src = buf;
  int in_single = 0, in_double = 0;
  args[argc] = dst;

  while (*src && argc < max_args - 1)
  {
    char c = *src++;
    if (c == '\\')
    {
      if (!in_single && !in_double)
      {
        if (*src)
          *dst++ = *src++;
      }
      else if (in_double)
      {
        if (*src == '"' || *src == '\\')
        {
          *dst++ = *src++;
        }
        else
        {
          *dst++ = '\\';
          if (*src)
            *dst++ = *src++;
        }
      }
      else
      {
        *dst++ = c;
      }
    }
    else if (c == '\'' && !in_double)
    {
      in_single = !in_single;
    }
    else if (c == '"' && !in_single)
    {
      in_double = !in_double;
    }
    else if (is_space(c) && !in_single && !in_double)
    {
      *dst++ = '\0';
      while (*src && is_space(*src))
        src++;
      if (*src)
      {
        argc++;
        args[argc] = dst;
      }
    }
    else
    {
      *dst++ = c;
    }
  }
  *dst = '\0';
  argc++;
  args[argc] = NULL;
  return argc;
}

/* split pipeline by | respecting quotes */
static int split_pipeline(const char *input, char *segments[], int max_segments)
{
  int count = 0;
  int in_single = 0, in_double = 0;
  char *buf = strdup(input);
  if (!buf)
    return 0;
  char *p = buf;
  segments[count++] = p;
  while (*p && count < max_segments)
  {
    if (*p == '\'' && !in_double)
      in_single = !in_single;
    else if (*p == '"' && !in_single)
      in_double = !in_double;
    else if (*p == '|' && !in_single && !in_double)
    {
      *p = '\0';
      p++;
      while (*p && is_space(*p))
        p++;
      if (*p)
        segments[count++] = p;
      continue;
    }
    p++;
  }
  return count;
}

/* extract redirections from args */

static void init_redir(Redir *r)
{
  r->stdout_file = NULL;
  r->stdout_append = 0;
  r->stderr_file = NULL;
  r->stderr_append = 0;
}

static int extract_redirect(char *args[], int argc, Redir *r)
{
  int outc = 0;
  for (int i = 0; i < argc; i++)
  {
    if ((strcmp(args[i], ">") == 0 || strcmp(args[i], "1>") == 0 ||
         strcmp(args[i], ">>") == 0 || strcmp(args[i], "1>>") == 0 ||
         strcmp(args[i], "2>") == 0 || strcmp(args[i], "2>>") == 0) &&
        i + 1 < argc)
    {
      int is_stderr = (args[i][0] == '2');
      int append = (strstr(args[i], ">>") != NULL);
      if (is_stderr)
      {
        r->stderr_file = args[i + 1];
        r->stderr_append = append;
      }
      else
      {
        r->stdout_file = args[i + 1];
        r->stdout_append = append;
      }
      i++;
    }
    else
    {
      args[outc++] = args[i];
    }
  }
  args[outc] = NULL;
  return outc;
}

static int open_redir_file(const char *file, int append)
{
  int flags = O_WRONLY | O_CREAT;
  if (append)
    flags |= O_APPEND;
  else
    flags |= O_TRUNC;
  int fd = open(file, flags, 0666);
  return fd;
}

/* ---------- Pipelines ---------- */

static void run_pipeline(char *segments[], int seg_count)
{
  if (seg_count <= 0)
    return;
  int pipes[seg_count - 1][2];
  for (int i = 0; i < seg_count - 1; i++)
  {
    if (pipe(pipes[i]) < 0)
    {
      perror("pipe");
      return;
    }
  }

  pid_t pids[seg_count];

  for (int i = 0; i < seg_count; i++)
  {
    char *seg = segments[i];
    char *args[MAX_ARGS];
    int argc = parse_args(seg, args, MAX_ARGS);
    if (argc == 0)
    {
      pids[i] = -1;
      continue;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
      perror("fork");
      return;
    }
    else if (pid == 0)
    {
      if (i > 0)
      {
        dup2(pipes[i - 1][0], STDIN_FILENO);
      }
      if (i < seg_count - 1)
      {
        dup2(pipes[i][1], STDOUT_FILENO);
      }
      for (int j = 0; j < seg_count - 1; j++)
      {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }

      /* Built‑ins inside pipelines must run in the child */
      if (is_builtin(args[0]))
      {
        Redir r;
        init_redir(&r);

        if (strcmp(args[0], "echo") == 0)
        {
          builtin_echo(args, argc, &r);
        }
        else if (strcmp(args[0], "type") == 0)
        {
          builtin_type(args, argc, &r);
        }
        else if (strcmp(args[0], "pwd") == 0)
        {
          builtin_pwd(&r);
        }
        else if (strcmp(args[0], "cd") == 0)
        {
          builtin_cd(args, argc);
        }
        else if (strcmp(args[0], "jobs") == 0)
        {
          builtin_jobs();
        }
        else if (strcmp(args[0], "history") == 0)
        {
          builtin_history(args, argc);
        }
        else if (strcmp(args[0], "complete") == 0)
        {
          builtin_complete(args, argc);
        }
        else if (strcmp(args[0], "declare") == 0)
        {
          builtin_declare(args, argc);
        }
        _exit(0);
      }

      /* External command */
      char *path = find_in_path(args[0]);
      if (!path)
      {
        fprintf(stderr, "%s: command not found\n", args[0]);
        _exit(127);
      }
      execv(path, args);
      perror("execv");
      _exit(127);
    }
    else
    {
      pids[i] = pid;
    }
  }

  for (int i = 0; i < seg_count - 1; i++)
  {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }

  for (int i = 0; i < seg_count; i++)
  {
    if (pids[i] > 0)
    {
      waitpid(pids[i], NULL, 0);
    }
  }
}

/* ---------- Builtins ---------- */
static int is_builtin(const char *cmd)
{
  const char *builtins[] = {
      "echo", "exit", "type", "pwd", "cd",
      "complete", "jobs", "history", "declare", NULL};
  for (int i = 0; builtins[i]; i++)
  {
    if (strcmp(builtins[i], cmd) == 0)
      return 1;
  }
  return 0;
}

static void builtin_echo(char *args[], int argc, Redir *r)
{
  FILE *out = stdout;
  int fd = -1;
  if (r->stdout_file)
  {
    fd = open_redir_file(r->stdout_file, r->stdout_append);
    if (fd >= 0)
      out = fdopen(fd, r->stdout_append ? "a" : "w");
  }

  FILE *errout = stderr;
  int errfd = -1;
  if (r->stderr_file)
  {
    errfd = open_redir_file(r->stderr_file, r->stderr_append);
    if (errfd >= 0)
    {
      FILE *tmp = fdopen(errfd, r->stderr_append ? "a" : "w");
      if (tmp)
      {
        errout = tmp;
      }
      else
      {
        /* fdopen failed: close fd to avoid leak and keep stderr as errout */
        close(errfd);
        errfd = -1;
      }
    }
  }

  for (int i = 1; i < argc; i++)
  {
    fprintf(out, "%s", args[i]);
    if (i + 1 < argc)
      fputc(' ', out);
  }
  fputc('\n', out);

  if (fd >= 0)
  {
    fclose(out); /* closes fd too */
  }

  if (errfd >= 0)
  {
    fclose(errout); /* close redirected stderr only if we opened it */
  }
}

static void builtin_type(char *args[], int argc, Redir *r)
{
  if (argc < 2)
    return;
  FILE *out = stdout;
  int fd = -1;
  if (r->stdout_file)
  {
    fd = open_redir_file(r->stdout_file, r->stdout_append);
    if (fd >= 0)
      out = fdopen(fd, r->stdout_append ? "a" : "w");
  }
  const char *arg = args[1];
  if (is_builtin(arg))
  {
    fprintf(out, "%s is a shell builtin\n", arg);
  }
  else
  {
    char *path = find_in_path(arg);
    if (path)
    {
      fprintf(out, "%s is %s\n", arg, path);
      free(path);
    }
    else
    {
      fprintf(out, "%s: not found\n", arg);
    }
  }
  if (fd >= 0)
    fclose(out);
}

static void builtin_pwd(Redir *r)
{
  FILE *out = stdout;
  int fd = -1;
  if (r->stdout_file)
  {
    fd = open_redir_file(r->stdout_file, r->stdout_append);
    if (fd >= 0)
      out = fdopen(fd, r->stdout_append ? "a" : "w");
  }
  char *cwd = getcwd(NULL, 0);
  if (cwd)
  {
    fprintf(out, "%s\n", cwd);
    free(cwd);
  }
  if (fd >= 0)
    fclose(out);
}

static void builtin_cd(char *args[], int argc)
{
  const char *target = NULL;
  if (argc < 2)
  {
    target = getenv("HOME");
    if (!target)
      target = "/";
  }
  else
  {
    if (strcmp(args[1], "~") == 0)
    {
      target = getenv("HOME");
      if (!target)
        target = "/";
    }
    else
    {
      target = args[1];
    }
  }
  if (chdir(target) != 0)
  {
    fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
  }
}

static void builtin_jobs(void)
{
  print_jobs();
}

static void builtin_history(char *args[], int argc)
{
  if (argc >= 2 && strcmp(args[1], "-r") == 0)
  {
    if (argc >= 3)
    {
      load_history_from_file(args[2]);
    }
  }
  else if (argc >= 2 && strcmp(args[1], "-w") == 0)
  {
    if (argc >= 3)
    {
      save_history_to_file(args[2]);
    }
  }
  else if (argc >= 2 && strcmp(args[1], "-a") == 0)
  {
    if (argc >= 3)
    {
      append_history_to_file(args[2]);
    }
  }
  else
  {
    size_t start = 0;
    if (argc >= 2)
    {
      int n = atoi(args[1]);
      if (n > 0 && (size_t)n < history_len)
      {
        start = history_len - n;
      }
    }
    for (size_t i = start; i < history_len; i++)
    {
      printf("%4zu  %s\n", i + 1, history[i]);
    }
  }
}

static void builtin_complete(char *args[], int argc)
{
  if (argc >= 3 && strcmp(args[1], "-p") == 0)
  {
    const char *cmd_name = args[2];
    const char *script = lookup_completion(cmd_name);
    if (script)
    {
      printf("complete -C '%s' %s\n", script, cmd_name);
    }
    else
    {
      printf("complete: %s: no completion specification\n", cmd_name);
    }
  }
  else if (argc >= 4 && strcmp(args[1], "-C") == 0)
  {
    const char *script = args[2];
    const char *cmd_name = args[3];
    register_completion(cmd_name, script);
  }
  else if (argc >= 3 && strcmp(args[1], "-r") == 0)
  {
    const char *cmd_name = args[2];
    for (int i = 0; i < completion_count; i++)
    {
      if (strcmp(completions[i].command, cmd_name) == 0)
      {
        completions[i] = completions[completion_count - 1];
        completion_count--;
        break;
      }
    }
  }
}

static void builtin_declare(char *args[], int argc)
{
  if (argc >= 3 && strcmp(args[1], "-p") == 0)
  {
    const char *var_name = args[2];
    const char *val = get_shell_var(var_name);
    if (val)
    {
      printf("declare -- %s=\"%s\"\n", var_name, val);
    }
    else
    {
      fprintf(stderr, "declare: %s: not found\n", var_name);
    }
  }
  else
  {
    for (int i = 1; i < argc; i++)
    {
      char original[256];
      snprintf(original, sizeof(original), "%s", args[i]);

      char *eq = strchr(args[i], '=');
      if (!eq)
        continue;

      *eq = '\0';
      const char *name = args[i];
      const char *value = eq + 1;

      if (!isalpha((unsigned char)name[0]) && name[0] != '_')
      {
        fprintf(stderr, "declare: `%s': not a valid identifier\n", original);
        continue;
      }

      int valid = 1;
      for (const char *p = name + 1; *p; p++)
      {
        if (!isalnum((unsigned char)*p) && *p != '_')
        {
          valid = 0;
          break;
        }
      }

      if (!valid)
      {
        fprintf(stderr, "declare: `%s': not a valid identifier\n", original);
        continue;
      }

      set_shell_var(name, value);
    }
  }
}

/* ---------- Execution ---------- */

static void execute_external(char *args[], int background, Redir *r)
{
  char *path = find_in_path(args[0]);
  if (!path)
  {
    fprintf(stderr, "%s: command not found\n", args[0]);
    return;
  }

  pid_t pid = fork();
  if (pid < 0)
  {
    perror("fork");
    free(path);
    return;
  }
  else if (pid == 0)
  {
    if (r->stdout_file)
    {
      int fd = open_redir_file(r->stdout_file, r->stdout_append);
      if (fd >= 0)
      {
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
    }
    if (r->stderr_file)
    {
      int fd = open_redir_file(r->stderr_file, r->stderr_append);
      if (fd >= 0)
      {
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
    execv(path, args);
    perror("execv");
    _exit(127);
  }
  else
  {
    free(path);
    if (background)
    {
      char cmdline[INPUT_BUFFER_SIZE] = {0};
      for (int i = 0; args[i]; i++)
      {
        strcat(cmdline, args[i]);
        if (args[i + 1])
          strcat(cmdline, " ");
      }
      add_job(pid, cmdline);
    }
    else
    {
      int status;
      waitpid(pid, &status, 0);
    }
  }
}

/* ---------- Programmable completion (single completion) ---------- */
static void handle_tab_completion(char *line)
{
  /* Normalize input */
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
  {
    line[--len] = '\0';
  }

  for (size_t i = 0; i < len; i++)
  {
    if (line[i] == '\t')
    {
      memmove(&line[i], &line[i + 1], len - i);
      len--;
      i--;
    }
  }

  size_t cursor = strlen(line);

  /* Find start of current word */
  size_t word_start = cursor;
  while (word_start > 0 && !is_space((unsigned char)line[word_start - 1]))
  {
    word_start--;
  }

  size_t prefix_start = word_start;
  size_t prefix_len = cursor - word_start;

  char prefix[INPUT_BUFFER_SIZE];
  snprintf(prefix, sizeof(prefix), "%.*s", (int)prefix_len, line + prefix_start);

  /* Determine command name (first word) */
  char cmd_name[128] = {0};
  {
    size_t i = 0;
    while (line[i] && !is_space((unsigned char)line[i]) && i < sizeof(cmd_name) - 1)
    {
      cmd_name[i] = line[i];
      i++;
    }
    cmd_name[i] = '\0';
  }
  /* Extract previous word */
  char prev_word[INPUT_BUFFER_SIZE] = {0};

  if (prefix_start > 0)
  {
    /* Find start of previous word */
    size_t end = prefix_start - 1;

    /* Skip spaces */
    while (end > 0 && is_space((unsigned char)line[end]))
    {
      end--;
    }

    /* Find beginning of previous word */
    size_t begin = end;
    while (begin > 0 && !is_space((unsigned char)line[begin - 1]))
    {
      begin--;
    }

    if (end >= begin)
    {
      snprintf(prev_word, sizeof(prev_word), "%.*s",
               (int)(end - begin + 1), line + begin);
    }
  }

  /* Determine directory to scan */
  const char *scan_dir = ".";
  const char *slash = strrchr(prefix, '/');
  char dirbuf[1024];

  const char *file_prefix = prefix;
  if (slash)
  {
    size_t dlen = slash - prefix;
    snprintf(dirbuf, sizeof(dirbuf), "%.*s", (int)dlen, prefix);
    scan_dir = dirbuf;
    file_prefix = slash + 1;
  }

  /* Compute insertion range: only replace file_prefix */
  size_t rel = (size_t)(file_prefix - prefix);
  size_t insert_start = prefix_start + rel;
  size_t insert_len = prefix_len - rel;

  /* Collect matches (programmable or filename) */
  char *matches[MAX_COMPLETIONS];
  int mcount = 0;

  const char *script = lookup_completion(cmd_name);
  int from_script = (script != NULL);
  char script_output[4096] = {0};

  if (script)
  {
    run_completer_script(
        script,
        cmd_name,
        file_prefix,
        prev_word,
        line,
        cursor,
        script_output,
        sizeof(script_output));

    char *tok = strtok(script_output, "\n");
    while (tok && mcount < MAX_COMPLETIONS)
    {
      matches[mcount++] = strdup(tok);
      tok = strtok(NULL, "\n");
    }
  }
  else
  {
    find_file_matches(scan_dir, file_prefix, matches, &mcount);
  }

  /* No matches → bell */
  if (mcount == 0)
  {
    putchar('\a');
    fflush(stdout);
    return;
  }

  /* Compute LCP */
  int lcp = compute_lcp(matches, mcount);

  /* LCP insertion (only when multiple matches) */
  if (mcount > 1 && lcp > (int)strlen(file_prefix))
  {
    char insert[1024] = {0};
    snprintf(insert, sizeof(insert), "%.*s", lcp, matches[0]);

    if (insert_at_prefix_and_redraw(line, insert_start, insert_len, insert))
    {
      putchar('\a');
      fflush(stdout);
    }

    last_completion_was_multi = 0;

    for (int i = 0; i < mcount; i++)
    {
      free(matches[i]);
    }

    return;
  }

  /* Single match → insert with / or space */
  if (mcount == 1)
  {
    char insert[1024] = {0};
    snprintf(insert, sizeof(insert), "%s", matches[0]);

    if (from_script)
    {
      // If the match came from a script, we don't know if it's a directory or not, so we just add a space.
      strncat(insert, " ", sizeof(insert) - strlen(insert) - 1);
    }
    else
    {
      char fullpath[1024];
      if (strcmp(scan_dir, ".") == 0)
      {
        snprintf(fullpath, sizeof(fullpath), "%s", matches[0]);
      }
      else
      {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", scan_dir, matches[0]);
      }

      if (is_directory(fullpath))
      {
        strncat(insert, "/", sizeof(insert) - strlen(insert) - 1);
      }
      else
      {
        strncat(insert, " ", sizeof(insert) - strlen(insert) - 1);
      }
    }

    if (insert_at_prefix_and_redraw(line, insert_start, insert_len, insert))
    {
      putchar('\a');
      fflush(stdout);
    }

    last_completion_was_multi = 0;

    for (int i = 0; i < mcount; i++)
    {
      free(matches[i]);
    }

    return;
  }

  /* Multiple matches, first TAB → bell */
  if (!last_completion_was_multi)
  {
    putchar('\a');
    fflush(stdout);
    last_completion_was_multi = 1;

    for (int i = 0; i < mcount; i++)
    {
      free(matches[i]);
    }

    return;
  }

  /* Multiple matches, second TAB → show list */
  qsort(matches, mcount, sizeof(char *), cmp_strptr);
  show_matches(matches, mcount, line, scan_dir);

  last_completion_was_multi = 1;

  for (int i = 0; i < mcount; i++)
  {
    free(matches[i]);
  }
}

/* ---------- Main loop ---------- */
int main(void)
{
  memset(completions, 0, sizeof(completions));
  completion_count = 0;
  setbuf(stdout, NULL);
  const char *histfile = getenv("HISTFILE");

  if (histfile)
  {
    load_history_from_file(histfile);
  }

  char line[INPUT_BUFFER_SIZE];

  for (;;)
  {
    reap_jobs();
    printf("$ ");
    fflush(stdout);

    enable_raw_mode();
    if (read_input_line(line, sizeof(line)) < 0)
    {
      disable_raw_mode();
      if (histfile)
        save_history_to_file(histfile);
      break;
    }
    disable_raw_mode();

    /* Ignore empty lines */
    if (line[0] == '\0')
      continue;

    /* Simulate TAB completion trigger: if line ends with '\t' in tests */
    if (line[strlen(line) - 1] == '\t')
    {
      handle_tab_completion(line);
      continue;
    }

    add_history(line);

    /* Pipelines? */
    char *segments[64];
    int seg_count = split_pipeline(line, segments, 64);
    if (seg_count > 1)
    {
      run_pipeline(segments, seg_count);
      continue;
    }

    /* Single command */
    char *args[MAX_ARGS];
    int argc = parse_args(line, args, MAX_ARGS);
    if (argc == 0)
      continue;

    /* Background? */
    int background = 0;
    if (argc > 0 && strcmp(args[argc - 1], "&") == 0)
    {
      background = 1;
      args[--argc] = NULL;
    }

    /* Redirections */
    Redir r;
    init_redir(&r);
    argc = extract_redirect(args, argc, &r);
    if (argc == 0)
      continue;

    /* Expand variables in args */
    for (int i = 0; i < argc; i++)
    {
      char *expanded = expand_vars(args[i]);
      if (expanded)
      {
        args[i] = expanded;
      }
    }
    /* Remove empty arguments created by expansion */
    int out = 0;
    for (int i = 0; i < argc; i++)
    {
      if (args[i] && args[i][0] != '\0')
      {
        args[out++] = args[i];
      }
    }
    args[out] = NULL;
    argc = out;

    if (strcmp(args[0], "exit") == 0)
    {
      if (histfile)
      {
        save_history_to_file(histfile);
      }
      break;
    }
    else if (strcmp(args[0], "echo") == 0)
    {
      builtin_echo(args, argc, &r);
    }
    else if (strcmp(args[0], "type") == 0)
    {
      builtin_type(args, argc, &r);
    }
    else if (strcmp(args[0], "pwd") == 0)
    {
      builtin_pwd(&r);
    }
    else if (strcmp(args[0], "cd") == 0)
    {
      builtin_cd(args, argc);
    }
    else if (strcmp(args[0], "jobs") == 0)
    {
      builtin_jobs();
    }
    else if (strcmp(args[0], "history") == 0)
    {
      builtin_history(args, argc);
    }
    else if (strcmp(args[0], "complete") == 0)
    {
      builtin_complete(args, argc);
    }
    else if (strcmp(args[0], "declare") == 0)
    {
      builtin_declare(args, argc);
    }
    else
    {
      execute_external(args, background, &r);
    }

    /* free expanded args */
    for (int i = 0; i < argc; i++)
    {
      /* we allocated them in expand_vars; in a real shell you'd track ownership */
      /* here we just leak a bit or assume all args were expanded */
    }
  }

  return 0;
}
