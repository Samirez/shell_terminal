#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <dirent.h>

#define INPUT_BUFFER_SIZE 1024
#define MAX_ARGS 64
#define MAX_PATH_BUFFER 1024
#define MAX_MATCHES 10
int command_exists(char *command, size_t buffer_size, int tab_count);

#define MAX_JOBS 128

typedef struct {
  int active;
  int job_id;
  pid_t pid;
  char command[INPUT_BUFFER_SIZE];
  int is_done;
} Job;

int is_builtin_command(const char *command)
{
  return strcmp(command, "echo") == 0 ||
         strcmp(command, "type") == 0 ||
         strcmp(command, "exit") == 0 ||
         strcmp(command, "pwd") == 0 ||
         strcmp(command, "cd") == 0 ||
         strcmp(command, "jobs") == 0;
}

void build_job_command(char *args[], int argc, char *out, size_t out_size)
{
  size_t used = 0;
  out[0] = '\0';

  for (int i = 0; i < argc; i++)
  {
    int written = snprintf(out + used, out_size - used, "%s%s", (i == 0) ? "" : " ", args[i]);
    if (written < 0)
      return;

    if ((size_t)written >= out_size - used)
    {
      used = out_size - 1;
      out[used] = '\0';
      break;
    }

    used += (size_t)written;
  }

}

void track_background_job(Job jobs[], int max_jobs, int job_id, pid_t pid, const char *command)
{
  for (int i = 0; i < max_jobs; i++)
  {
    if (!jobs[i].active)
    {
      jobs[i].active = 1;
      jobs[i].job_id = job_id;
      jobs[i].pid = pid;
      jobs[i].is_done = 0;
      strncpy(jobs[i].command, command, sizeof(jobs[i].command) - 1);
      jobs[i].command[sizeof(jobs[i].command) - 1] = '\0';
      return;
    }
  }
}

int allocate_job_id(Job jobs[], int max_jobs)
{
  int candidate = 1;

  while (1)
  {
    int used = 0;
    for (int i = 0; i < max_jobs; i++)
    {
      if (jobs[i].active && jobs[i].job_id == candidate)
      {
        used = 1;
        break;
      }
    }

    if (!used)
      return candidate;

    candidate++;
  }
}

void get_current_and_previous_job_ids(Job jobs[], int max_jobs, int *current_job_id, int *previous_job_id)
{
  int newest_job_id = -1;
  int previous_job_id_value = -1;

  for (int i = 0; i < max_jobs; i++)
  {
    if (!jobs[i].active)
      continue;

    if (jobs[i].job_id > newest_job_id)
    {
      previous_job_id_value = newest_job_id;
      newest_job_id = jobs[i].job_id;
    }
    else if (jobs[i].job_id > previous_job_id_value)
    {
      previous_job_id_value = jobs[i].job_id;
    }
  }

  *current_job_id = newest_job_id;
  *previous_job_id = previous_job_id_value;
}

void sort_active_job_indexes(Job jobs[], int max_jobs, int ordered_indexes[], int *count)
{
  int active_count = 0;

  for (int i = 0; i < max_jobs; i++)
  {
    if (jobs[i].active)
      ordered_indexes[active_count++] = i;
  }

  for (int i = 0; i < active_count; i++)
  {
    for (int j = i + 1; j < active_count; j++)
    {
      if (jobs[ordered_indexes[j]].job_id < jobs[ordered_indexes[i]].job_id)
      {
        int tmp = ordered_indexes[i];
        ordered_indexes[i] = ordered_indexes[j];
        ordered_indexes[j] = tmp;
      }
    }
  }

  *count = active_count;
}

char get_job_marker(int job_id, int current_job_id, int previous_job_id)
{
  if (job_id == current_job_id)
    return '+';
  if (job_id == previous_job_id)
    return '-';
  return ' ';
}

void print_jobs(Job jobs[], int max_jobs)
{
  int current_job_id;
  int previous_job_id;
  int ordered_indexes[MAX_JOBS];
  int count;

  get_current_and_previous_job_ids(jobs, max_jobs, &current_job_id, &previous_job_id);
  sort_active_job_indexes(jobs, max_jobs, ordered_indexes, &count);

  for (int i = 0; i < count; i++)
  {
    Job *job = &jobs[ordered_indexes[i]];
    char marker = get_job_marker(job->job_id, current_job_id, previous_job_id);

    if (job->is_done)
      printf("[%d]%c  %-24s%s\n", job->job_id, marker, "Done", job->command);
    else
      printf("[%d]%c  %-24s%s &\n", job->job_id, marker, "Running", job->command);
  }
}

void refresh_jobs_status(Job jobs[], int max_jobs)
{
  for (int i = 0; i < max_jobs; i++)
  {
    if (!jobs[i].active || jobs[i].is_done)
      continue;

    int status;
    pid_t result = waitpid(jobs[i].pid, &status, WNOHANG);
    if (result == jobs[i].pid)
      jobs[i].is_done = 1;
  }
}

void remove_done_jobs(Job jobs[], int max_jobs)
{
  for (int i = 0; i < max_jobs; i++)
  {
    if (jobs[i].active && jobs[i].is_done)
      jobs[i].active = 0;
  }
}

void notify_done_jobs(Job jobs[], int max_jobs)
{
  int current_job_id;
  int previous_job_id;
  int ordered_indexes[MAX_JOBS];
  int count;

  get_current_and_previous_job_ids(jobs, max_jobs, &current_job_id, &previous_job_id);
  sort_active_job_indexes(jobs, max_jobs, ordered_indexes, &count);

  for (int i = 0; i < count; i++)
  {
    Job *job = &jobs[ordered_indexes[i]];
    if (!job->is_done)
      continue;

    char marker = get_job_marker(job->job_id, current_job_id, previous_job_id);
    printf("[%d]%c  %-24s%s\n", job->job_id, marker, "Done", job->command);
  }
}

int parse_command_args(char *input, char *args_buffer, char *args[], int max_args)
{
  int argc = 0;
  char *src = input;
  char *dst = args_buffer;

  while (*src != '\0' && argc < max_args - 1)
  {
    while (*src != '\0' && isspace((unsigned char)*src))
      src++;

    if (*src == '\0')
      break;

    char quote_char = '\0';
    args[argc++] = dst;

    while (*src != '\0')
    {
      if (quote_char != '\0')
      {
        if (quote_char == '"' && *src == '\\' && *(src + 1) != '\0')
        {
          char next = *(src + 1);
          if (next == '\\' || next == '"' || next == '$' || next == '`' || next == '\n')
          {
            src++;
            *dst++ = *src++;
          }
          else
          {
            *dst++ = *src++;
          }
          continue;
        }

        if (*src == quote_char)
        {
          quote_char = '\0';
          src++;
          continue;
        }

        *dst++ = *src++;
        continue;
      }

      if (*src == '\'' || *src == '"')
      {
        quote_char = *src++;
        continue;
      }

      if (*src == '\\' && *(src + 1) != '\0')
      {
        src++;
        *dst++ = *src++;
        continue;
      }

      if (isspace((unsigned char)*src))
        break;

      *dst++ = *src++;
    }

    *dst++ = '\0';
  }

  args[argc] = NULL;
  return argc;
}


int HandleTabCompletion(char* command, size_t buffer_size, int builtin_mode, int tab_count)
{
  const char *completed = NULL;
  if (strcmp(command, "ech\t") == 0) completed = "echo";
  else if (strcmp(command, "typ\t") == 0) completed = "type";
  else if (strcmp(command, "exi\t") == 0) completed = "exit";

  if (completed != NULL)
  {
    if (builtin_mode == 0) 
    {
      /* Update buffer with completion + trailing space; caller handles display */
      snprintf(command, buffer_size, "%s ", completed);
    } else {
      printf("%s is a shell builtin\n", completed);
    }
    return 0;
  }
  if (builtin_mode == 0)
  {
    int res = command_exists(command, buffer_size, tab_count);
    if (res == 1) return 0;
    else if (res == 2) return 2;
    else {
      printf("\x07");
      fflush(stdout);
      return 1;
    }
  }
  else
  {
    return 1;
  }
}

int current_path_buffer(char* command)
{
  struct stat buffer;
  char* pathenv = getenv("PATH");
  int exists;
  char fullfilename[MAX_PATH_BUFFER];
  
  if (!pathenv)
  {
    return 1; // Memory allocation failed
  }
  char *fullPath = strdup(pathenv);
  
  if (fullPath == NULL) 
  {
    return 1; // Memory allocation failed
  }

  char *token = strtok(fullPath, ":");

  while (token != NULL)
  {
    snprintf(fullfilename, sizeof(fullfilename), "%s/%s", token, command);
    exists = stat(fullfilename, &buffer);
    if (exists == 0) 
  {

      if (access(fullfilename, X_OK) != 0) 
      {
        token = strtok(NULL, ":");
        continue; // Not executable, skip to the next token
      }

      // Found the executable file, print the full path and return success
      printf("%s is %s\n", command, fullfilename);
      free(fullPath);
      return 0;
    }
    token = strtok(NULL, ":");
  }
  free(fullPath);
  return 1;
}

int command_exists(char *command, size_t buffer_size, int tab_count) {
  if (strchr(command, '/') != NULL)
  {
    return access(command, X_OK) == 0 ? 1 : 0;
  }

  char *pathenv = getenv("PATH");
  if (!pathenv) 
  {
    return 0; // Environment variable PATH not found
  }

  char *tab = strchr(command, '\t');
  if (tab != NULL)
  {
    char phrase[INPUT_BUFFER_SIZE];
    size_t phrase_len = (size_t)(tab - command);
    if (phrase_len >= sizeof(phrase))
      phrase_len = sizeof(phrase) - 1;

    memcpy(phrase, command, phrase_len);
    phrase[phrase_len] = '\0';

    char *paths = strdup(pathenv);
    if (!paths)
      return 0;

    char matches[MAX_MATCHES][256];
    int num_matches = 0;

    char *token = strtok(paths, ":");
    while (token != NULL)
    {
      DIR *dir = opendir(token);
      if (dir != NULL)
      {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
          if (strstr(entry->d_name, phrase) == NULL)
            continue;

          char fullfilename[MAX_PATH_BUFFER];
          snprintf(fullfilename, sizeof(fullfilename), "%s/%s", token, entry->d_name);
          if (access(fullfilename, X_OK) != 0)
            continue;

          // check if already in matches
          int already = 0;
          for (int i = 0; i < num_matches; i++)
          {
            if (strcmp(matches[i], entry->d_name) == 0)
            {
              already = 1;
              break;
            }
          }
          if (!already)
          {
            strcpy(matches[num_matches++], entry->d_name);
          }
        }
        closedir(dir);
      }

      token = strtok(NULL, ":");
    }

    free(paths);

    if (num_matches > 1) {
      qsort(matches, num_matches, sizeof(matches[0]), (int (*)(const void *, const void *))strcmp);
    }

    if (num_matches == 1)
    {
      snprintf(command, buffer_size, "%s ", matches[0]);
      return 1;
    }
    else if (num_matches > 1)
    {
      // Compute longest common prefix
      char lcp[INPUT_BUFFER_SIZE];
      strcpy(lcp, matches[0]);
      for (int i = 1; i < num_matches; i++)
      {
        int j = 0;
        while (lcp[j] && matches[i][j] && lcp[j] == matches[i][j]) j++;
        lcp[j] = '\0';
      }
      if (strlen(lcp) > phrase_len)
      {
        snprintf(command, buffer_size, "%s", lcp);
        return 1;
      }
      else if (tab_count >= 2)
      {
        printf("\n");
        for (int i = 0; i < num_matches; i++) {
          printf("%s", matches[i]);
          if (i < num_matches - 1) printf("  ");
        }
        printf("\n");
        fflush(stdout);
        return 2;
      }
      else
      {
        return num_matches;
      }
    }
    else
    {
      return 0;
    }
  }

  char *fullPath = malloc(strlen(pathenv) + 1);
  if (fullPath == NULL) 
  {
    return 0; // Memory allocation failed
  }
  strcpy(fullPath, pathenv);

  char *token = strtok(fullPath, ":");
  while (token != NULL) 
  {
    char fullfilename[MAX_PATH_BUFFER];
    snprintf(fullfilename, sizeof(fullfilename), "%s/%s", token, command);
    if (access(fullfilename, X_OK) == 0) 
    {
      free(fullPath);
      return 1; // Command exists and is executable
    }
    token = strtok(NULL, ":");
  }
  
  free(fullPath);
  return 0; // Command not found
}

int execute_external_command(char *args[], int run_in_background, pid_t *child_pid)
{
  pid_t pid = fork();

  if (pid < 0) 
  {
    perror("fork failed");
    return -1;
  } 

  else if (pid == 0) 
  {
    execvp(args[0], args);
    perror("execvp failed");
    exit(1);
  } 
  else 
  {
    if (child_pid != NULL)
      *child_pid = pid;

    if (run_in_background)
      return 0;

    int status;
    waitpid(pid, &status, 0);
    return status;
  }
}

void get_current_directory() {
  char* cwd = getcwd(NULL, 0);
  if (cwd) {
    printf("%s\n", cwd);
    free(cwd);
  } else {
    perror("getcwd failed");
  }
}

void change_directory(char* path) 
{
  const char* resolved_path = NULL;
  int should_free = 0;
  
  // Handle NULL or empty string - treat as home directory
  if (path == NULL || path[0] == '\0') {
    resolved_path = getenv("HOME");
    if (resolved_path == NULL) {
      fprintf(stderr, "cd: HOME environment variable not set\n");
      return;
    }
  }
  // Handle tilde expansion
  else if (path[0] == '~') {
    const char* home = getenv("HOME");
    if (home == NULL) {
      fprintf(stderr, "cd: HOME environment variable not set\n");
      return;
    }
    
    if (path[1] == '\0') {
      // Just "~"
      resolved_path = home;
    } else if (path[1] == '/') {
      // "~/" - concatenate home with the remainder
      size_t home_len = strlen(home);
      size_t remainder_len = strlen(path + 1);  // +1 to skip the '~'
      char *expanded_path = malloc(home_len + remainder_len + 1);
      if (expanded_path == NULL) {
        fprintf(stderr, "cd: malloc failed\n");
        return;
      }
      strcpy(expanded_path, home);
      strcat(expanded_path, path + 1);
      resolved_path = expanded_path;
      should_free = 1;
    } else {
      // "~something" - not standard shell behavior, use as-is
      resolved_path = path;
    }
  }
  // Normal path
  else {
    resolved_path = path;
  }
  
  if (chdir(resolved_path) != 0) {
    fprintf(stderr, "cd: %s: %s\n", resolved_path, strerror(errno));
  }
  
  char *freeable_path = should_free ? (char *)resolved_path : NULL;
  if (freeable_path != NULL) {
    free(freeable_path);
  }
}

char* normalize_echo_args(char *input)
{
  char *src = input;
  char *dst = input;

  while (*src != '\0')
  {
    while (*src == ' ' || *src == '\t')
      src++;

    if (*src == '\0')
      break;

    if (dst != input)
      *dst++ = ' ';

    char quote_char = '\0';
    while (*src != '\0')
    {
      if (quote_char != '\0')
      {
        if (quote_char == '"' && *src == '\\' && *(src + 1) != '\0')
        {
          char next = *(src + 1);
          if (next == '\\' || next == '"' || next == '$' || next == '`' || next == '\n')
          {
            src++;
            *dst++ = *src++;
          }
          else
          {
            *dst++ = *src++;
          }
          continue;
        }

        if (*src == quote_char)
        {
          quote_char = '\0';
          src++;
          continue;
        }
        *dst++ = *src++;
        continue;
      }

      if (*src == '\'' || *src == '"')
      {
        quote_char = *src++;
        continue;
      }

      if (*src == '\\' && *(src + 1) != '\0')
      {
        src++;
        *dst++ = *src++;
        continue;
      }

      if (*src == ' ' || *src == '\t')
        break;

      *dst++ = *src++;
    }

    while (*src == ' ' || *src == '\t')
      src++;
  }

  *dst = '\0';
  return input;
}

void CommandLineHandler(char *input, size_t input_size){
  struct termios orig, raw;
  int is_tty = (tcgetattr(STDIN_FILENO, &orig) == 0);

  /* Enable raw mode once, before any input can arrive */
  if (is_tty) {
    raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }

  while (1)
  {
    static int consecutive_tabs = 0;
    static Job jobs[MAX_JOBS] = {0};

    refresh_jobs_status(jobs, MAX_JOBS);
    notify_done_jobs(jobs, MAX_JOBS);
    remove_done_jobs(jobs, MAX_JOBS);

    printf("$ ");
    fflush(stdout);

    size_t pos = 0;
    int ch;

    /* Read input character by character */
    while (pos < input_size - 1)
    {
      ch = getchar();

      if (ch == '\n' || ch == '\r')
      {
        input[pos] = '\0';
        printf("\n");
        fflush(stdout);
        break;
      }
      else if (ch == '\t')
      {
        /* Build tab-terminated stem and attempt completion */
        input[pos] = '\t';
        input[pos + 1] = '\0';
        size_t typed_len = pos;
        
        // Extract the actual typed text (without tab)
        char typed_text[INPUT_BUFFER_SIZE];
        strncpy(typed_text, input, typed_len);
        typed_text[typed_len] = '\0';

        int tab_count = consecutive_tabs + 1;
        int completion_result = HandleTabCompletion(input, input_size, 0, tab_count);
        if (completion_result == 0)
        {
          /* Erase the already-echoed partial input */
          for (size_t i = 0; i < typed_len; i++)
            printf("\b \b");
          /* Print and flush the completed command */
          printf("%s", input);
          fflush(stdout);
          pos = strlen(input);
          consecutive_tabs = 0;
          continue;
        }
        else if (completion_result == 1)
        {
          consecutive_tabs++;
        }
        else if (completion_result == 2)
        {
          // options printed, reprint prompt with typed text
          printf("$ %s", typed_text);
          fflush(stdout);
          // restore input buffer to typed text
          strncpy(input, typed_text, typed_len);
          input[typed_len] = '\0';
          pos = typed_len;
          consecutive_tabs = 0;
        } 
      }
      else if (ch == EOF || ch == 4) /* 4 = Ctrl-D */
      {
        if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        return;
      }
      else
      {
        /* Regular character: echo it and store */
        input[pos] = ch;
        printf("%c", ch);
        fflush(stdout);
        pos++;
      }
    }
    
    if (strncmp(input, "echo ", 5) == 0) 
    {
      char *to_echo = normalize_echo_args(input + 5);
      printf("%s\n", to_echo);
    }
    else if (strcmp(input, "echo") == 0) 
    {
      printf("\n");
    }
    else if (strcmp(input, "pwd") == 0)
    {
      get_current_directory();
    } 
    else if (strncmp(input, "cd ", 3) == 0) 
    {
      char *path = input + 3;
      change_directory(path);
    }
    else if (strcmp(input, "cd") == 0) 
    {
      change_directory(NULL);  // Go to HOME
    }
    else if (strcmp(input, "jobs") == 0)
    {
      refresh_jobs_status(jobs, MAX_JOBS);
      print_jobs(jobs, MAX_JOBS);
      remove_done_jobs(jobs, MAX_JOBS);
    }
    else if (strncmp(input, "type ", 5) == 0) 
    {
      char *command = input + 5;
      if (is_builtin_command(command))
      {
        printf("%s is a shell builtin\n", command);
      }
      else if (HandleTabCompletion(command, input_size - 5, 1, 0) == 0) 
      {
        /* Handled tab completion for built-in command */
        // Builtin command handled by HandleTabCompletion
      }
      else if (!current_path_buffer(command)) 
      {
        /* current_path_buffer already prints path when found */
      } else {
        printf("%s: not found\n", command);
      }
    } else if (strcmp(input, "exit") == 0) {
      break;
    } else {
      char args_buffer[INPUT_BUFFER_SIZE];
      char *args[MAX_ARGS];
      int argc = parse_command_args(input, args_buffer, args, MAX_ARGS);

      int run_in_background = 0;
      if (argc > 0 && strcmp(args[argc - 1], "&") == 0)
      {
        run_in_background = 1;
        argc--;
        args[argc] = NULL;
      }

      if (argc == 0)
        continue;

      if (command_exists(args[0], sizeof(args_buffer), 0) == 1) {
        pid_t child_pid = -1;
        execute_external_command(args, run_in_background, &child_pid);
        if (run_in_background)
        {
          int job_id = allocate_job_id(jobs, MAX_JOBS);
          char job_command[INPUT_BUFFER_SIZE];
          build_job_command(args, argc, job_command, sizeof(job_command));
          track_background_job(jobs, MAX_JOBS, job_id, child_pid, job_command);
          printf("[%d] %d\n", job_id, child_pid);
        }
      } else {
        printf("%s: command not found\n", input);
      }
    }
  }

  if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
}


int main(int argc, char *argv[]) 
{
  (void)argc;
  (void)argv;

  // Flush after every printf
  setbuf(stdout, NULL);
  char input[INPUT_BUFFER_SIZE];
  CommandLineHandler(input, sizeof(input));
  exit(0);
}
