#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <dirent.h>

int command_exists(char *command, size_t buffer_size, int tab_count);


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
  const char* name = command;
  struct stat buffer;
  char* pathenv = getenv("PATH");
  int exists;
  char* fileOrDirectory = command;
  char fullfilename[1024];
  
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
  char *pathenv = getenv("PATH");
  if (!pathenv) 
  {
    return 0; // Environment variable PATH not found
  }

  char *tab = strchr(command, '\t');
  if (tab != NULL)
  {
    char phrase[1024];
    size_t phrase_len = (size_t)(tab - command);
    if (phrase_len >= sizeof(phrase))
      phrase_len = sizeof(phrase) - 1;

    memcpy(phrase, command, phrase_len);
    phrase[phrase_len] = '\0';

    char *paths = strdup(pathenv);
    if (!paths)
      return 0;

    char matches[10][256];
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

          char fullfilename[1024];
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
      char lcp[1024];
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
    char fullfilename[1024];
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

int execute_external_command(char *command)
{
  pid_t pid = fork();

  if (pid < 0) 
  {
    perror("fork failed");
    return -1;
  } 

  else if (pid == 0) 
  {
    char input_copy[1024];
    strncpy(input_copy, command, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';
    int argc = 0;
    char *args[64];
    char *token = strtok(input_copy, " ");

    while (token != NULL && argc < 63) 
    {
      args[argc++] = token;
      token = strtok(NULL, " ");
    }

    args[argc] = NULL;
    execvp(args[0], args);
    perror("execvp failed");
    exit(1);
  } 
  else 
  {
    // Parent process
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
  char* resolved_path = NULL;
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
      resolved_path = (char *)home;
    } else if (path[1] == '/') {
      // "~/" - concatenate home with the remainder
      size_t home_len = strlen(home);
      size_t remainder_len = strlen(path + 1);  // +1 to skip the '~'
      resolved_path = malloc(home_len + remainder_len + 1);
      if (resolved_path == NULL) {
        fprintf(stderr, "cd: malloc failed\n");
        return;
      }
      strcpy(resolved_path, home);
      strcat(resolved_path, path + 1);
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
  
  if (should_free) {
    free(resolved_path);
  }
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
        char typed_text[1024];
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
      printf("%s\n", input + 5);
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
    else if (strncmp(input, "type ", 5) == 0) 
    {
      char *command = input + 5;
      if (strcmp(command, "echo") == 0 || 
          strcmp(command, "type") == 0 || 
          strcmp(command, "exit") == 0 ||
          strcmp(command, "pwd") == 0 ||
          strcmp(command, "cd") == 0)
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
      char command_copy[1024];
      strcpy(command_copy, input);
      char *command = strtok(command_copy, " ");

      if (command && command_exists(command, sizeof(command_copy), 0) == 1) {
        execute_external_command(input);
      } else {
        printf("%s: command not found\n", input);
      }
    }
  }

  if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
}


int main(int argc, char *argv[]) 
{
  // Flush after every printf
  setbuf(stdout, NULL);
  char input[1024];
  CommandLineHandler(input, sizeof(input));
  exit(0);
}
