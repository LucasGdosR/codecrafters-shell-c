#include <assert.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*=================================================================================================
  DEFINES
=================================================================================================*/

#ifdef _WIN32
#define PATH_LIST_SEPARATOR ";"
#else
#define PATH_LIST_SEPARATOR ":"
#endif
#define ARENA_DEFAULT_SIZE 8192
#define MAX_CWD_SIZE 1024

/*=================================================================================================
  STRUCTS
=================================================================================================*/

typedef struct str {
  char *data;
  size_t len;
} str;

typedef struct arena {
  char *data;
  size_t len;
  size_t capacity;
} arena;

/*=================================================================================================
  ENUMS
=================================================================================================*/

enum Builtins {
  PWD,
  Echo,
  Type,
  Exit,
  Builtins_Size,
};

/*=================================================================================================
  GLOBALS
=================================================================================================*/

str PATH;
char *builtins[] = {[PWD]="pwd", [Echo]="echo", [Type]="type", [Exit]="exit"};

/*=================================================================================================
  FUNCTIONS
=================================================================================================*/

void arena_init(arena *arena, size_t size);
char* arena_push_string(arena *arena, size_t size);
void arena_reset(arena *arena);
char* find_executable(str *target, arena *allocator);
void type_builtin(str *string, arena *allocator);

int main(int argc, char *argv[])
{
  setbuf(stdout, NULL);
  str buffer = {0};
  PATH.data = getenv("PATH");
  PATH.len = strlen(PATH.data);
  arena temp_allocator = {0};
  arena_init(&temp_allocator, ARENA_DEFAULT_SIZE);

  // REPL
  for (;;)
  {
    arena_reset(&temp_allocator);
    printf("$ ");

    // Read:
    size_t len = getline(&buffer.data, &buffer.len, stdin);
    if (len == -1) // EOF
      break;

    // Eval-Print: builtins:
    // pwd
    if (len == 4 && strcmp(buffer.data, "pwd\n") == 0)
    {
      char *cwd = arena_push_string(&temp_allocator, MAX_CWD_SIZE);
      getcwd(cwd, MAX_CWD_SIZE);
      printf("%s\n", cwd);
    }
    // exit
    else if (len == 5 && strcmp(buffer.data, "exit\n") == 0)
      exit(0);
    // echo
    else if (len >= 5 && strncmp(buffer.data, "echo ", 5) == 0)
    {
      printf("%s", buffer.data + 5);
    }
    // type
    else if (len >= 5 && strncmp(buffer.data, "type ", 5) == 0)
    {
      str string = { buffer.data + 5, len - 5 };
      type_builtin(&string, &temp_allocator);
    }
    // executable
    else
    {
      // Remove trailing '\n'
      buffer.data[len - 1] = '\0';

      // Keep pointer to the start of the buffer for future iterations
      char *bp = buffer.data;

      str executable = {strsep(&bp, " ")};
      executable.len = strlen(executable.data);
      char *full_path = find_executable(&executable, &temp_allocator);
      if (full_path) {
        if (fork() == 0)
        {
          // Child process: get args and execute.
          int MAX_ARGS = 64;
          char *args[MAX_ARGS];
          char *arg;
          args[0] = executable.data;
          int i = 1;
          while ((arg = strsep(&bp, " "))) {
            // Guard against multiple spaces.
            if (arg[0] != '\0')
              args[i++] = arg;
            assert(i < MAX_ARGS);
          }
          args[i] = NULL;
          execv(full_path, args);
        }
        // Original process
        else wait(NULL);
      }
      else printf("%s: command not found\n", buffer.data);
    }
  }

  return 0;
}

void type_builtin(str *string, arena *allocator)
{
  string->data[--string->len] = '\0';

  // Builtin case:
  // For now, all builtins have length <= 4.
  if (string->len <= 4)
  {
    for (int i = 0; i < Builtins_Size; ++i)
    {
      if (strcmp(string->data, builtins[i]) == 0)
      {
        printf("%s is a shell builtin\n", string->data);
        return;
      }
    }
  }

  // Executable case:
  char *full_path = find_executable(string, allocator);
  if (full_path)
    printf("%s is %s\n", string->data, full_path);
  // Default case:
  else printf("%s: not found\n", string->data);
}

char* find_executable(str *target, arena *allocator)
{
  // PATH is immutable, so make a mutable copy.
  char *path = arena_push_string(allocator, PATH.len+1);
  memcpy(path, PATH.data, PATH.len+1);

  char *dir_str;
  while ((dir_str = strsep(&path, PATH_LIST_SEPARATOR)))
  {
    DIR *dir = opendir(dir_str);
    struct dirent *entry;
    if (dir) while ((entry = readdir(dir)))
    {
      if (entry->d_type == DT_REG && strcmp(entry->d_name, target->data) == 0)
      {
        unsigned long path_len = strlen(dir_str);
        char *full_path = arena_push_string(allocator, target->len + path_len + 2);
        memcpy(full_path, dir_str, path_len);
        full_path[path_len] = '/';
        memcpy(full_path + path_len + 1, target->data, target->len + 1);
        if (access(full_path, X_OK) == 0)
        {
          closedir(dir);
          return full_path;
        }
      }
    }
    closedir(dir);
  }
  return NULL;
}

void arena_init(arena *arena, size_t size)
{
  arena->data = malloc(size);
  arena->capacity = size;
  if (!arena->data)
  {
    fprintf(stderr, "Failed initializing arena with %lu bytes.\n", size);
    exit(1);
  }
}

void arena_reset(arena *arena)
{
  arena->len = 0;
}

char* arena_push_string(arena *arena, size_t size)
{
  if (arena->capacity < arena->len + size)
  {
    fprintf(stderr, "Arena overflowed.\n");
    exit(1);
  }
  char* result = arena->data + arena->len;
  arena->len += size;
  return result;
}