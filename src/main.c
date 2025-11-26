#include <assert.h>
#include <dirent.h>
#include <stdalign.h>
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

typedef struct args {
  size_t c;
  char *v[];
} args;

/*=================================================================================================
  ENUMS
=================================================================================================*/

enum Builtins {
  CD,
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
char *builtins[] = {[CD]="cd", [PWD]="pwd", [Echo]="echo", [Type]="type", [Exit]="exit"};

/*=================================================================================================
  FUNCTIONS
=================================================================================================*/

char* find_executable(str target, arena *allocator);
args* reset_arena_and_parse_args(arena *arena);
void type_builtin(char *arg, arena *allocator);

int is_whitespace(char c);
void trim(str* string);

void arena_init(arena *arena, size_t size);
void* arena_push(arena *arena, size_t alignment, size_t size);

int main(int argc, char *argv[])
{
  setbuf(stdout, NULL);
  PATH.data = getenv("PATH");
  PATH.len = strlen(PATH.data);
  arena temp_allocator = {0};
  arena_init(&temp_allocator, ARENA_DEFAULT_SIZE);

  // REPL
  for (;;)
  {
    printf("$ ");

    // Read:
    args *args = reset_arena_and_parse_args(&temp_allocator);
    if (!args->c) continue;

    // Eval-Print: builtins:
    if (strcmp(args->v[0], builtins[CD]) == 0)
    {
      if ((args->c == 1) || (strcmp(args->v[1], "~") == 0))
          chdir(getenv("HOME"));
      else if (chdir(args->v[1]))
          printf("cd: %s: No such file or directory\n", args->v[1]);
    }
    else if (strcmp(args->v[0], builtins[PWD]) == 0)
    {
      char *cwd = arena_push(&temp_allocator, alignof(char), MAX_CWD_SIZE);
      getcwd(cwd, MAX_CWD_SIZE);
      printf("%s\n", cwd);
    }
    else if (strcmp(args->v[0], builtins[Exit]) == 0)
      exit(0);
    else if (strcmp(args->v[0], builtins[Echo]) == 0)
    {
      size_t end = args->c - 1;
      if (end)
      {
        for (size_t i = 1; i < end; ++i)
          printf("%s ", args->v[i]);
        printf("%s\n", args->v[end]);
      }
    }
    else if (strcmp(args->v[0], builtins[Type]) == 0)
      type_builtin(args->v[1], &temp_allocator);
    else // executable
    {
      char *full_path = find_executable(
        (str){ args->v[0], strlen(args->v[0]) },
        &temp_allocator);
      if (full_path) {
        // Child process: get args and execute.
        if (fork() == 0) execv(full_path, args->v);
        // Original process
        else wait(NULL);
      }
      else printf("%s: command not found\n", args->v[0]);
    }
  }

  return 0;
}

void type_builtin(char *arg, arena *allocator)
{
  size_t len = strlen(arg);
  // Builtin case:
  // For now, all builtins have length <= 4.
  if (len <= 4)
  {
    for (int i = 0; i < Builtins_Size; ++i)
    {
      if (strcmp(arg, builtins[i]) == 0)
      {
        printf("%s is a shell builtin\n", arg);
        return;
      }
    }
  }

  // Executable case:
  char *full_path = find_executable((str){ arg, len }, allocator);
  if (full_path)
    printf("%s is %s\n", arg, full_path);
  // Default case:
  else printf("%s: not found\n", arg);
}

char* find_executable(str target, arena *allocator)
{
  // PATH is immutable, so make a mutable copy.
  char *path = arena_push(allocator, alignof(char), PATH.len+1);
  memcpy(path, PATH.data, PATH.len+1);

  char *dir_str;
  while ((dir_str = strsep(&path, PATH_LIST_SEPARATOR)))
  {
    DIR *dir = opendir(dir_str);
    struct dirent *entry;
    if (dir) while ((entry = readdir(dir)))
    {
      if (entry->d_type == DT_REG && strcmp(entry->d_name, target.data) == 0)
      {
        unsigned long path_len = strlen(dir_str);
        char *full_path = arena_push(allocator, alignof(char), target.len + path_len + 2);
        memcpy(full_path, dir_str, path_len);
        full_path[path_len] = '/';
        memcpy(full_path + path_len + 1, target.data, target.len + 1);
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

args* reset_arena_and_parse_args(arena *allocator)
{
  // This resets the arena by setting `allocator->len`.
  allocator->len = getline(&allocator->data, &allocator->capacity, stdin);
  if (allocator->len == -1) // EOF
    exit(0);

  // The '\n' is stripped from the line's end.
  str string = {allocator->data, allocator->len};
  trim(&string);

  // Push a slice to the arena. Fill `args->v` by pushing null terminated `char *token`s to the arena.
  args *args = arena_push(allocator, alignof(struct args), sizeof(size_t));
  if (string.len == 0)
  {
    args->c = 0;
    return args;
  }

  char *start = string.data, *end = string.data, *bound = string.data + string.len;
  size_t count = 0; // Keep count of tokens for `args->c`.
  int edge_case = 0; // Slow track dealing with quotes.
  while (end < bound)
  {
    switch (*end++) // Advance parser.
    {
      //case '"':
      case '\'':
        edge_case = 1;
        while (end < bound && *end++ != '\'');
        break;
      case ' ':
      case '\t':
        // Deal with quotes by overwriting everything but quotes.
        if (edge_case)
        {
          edge_case = 0;

          // Adjust token start to ignore quotes.
          while (*start++ == '\'');
          start--;

          // Overwrite the input buffer directly.
          char *left = start, *right = start, c;
          while (right < end)
            if ((c = *right++) != '\'')
              *left++ = c;

          // Add a null terminator.
          *(left - 1) = '\0';
        }
        else
        {
          // Replace ' ' by null terminator.
          *(end - 1) = '\0';
        }
        // Store a pointer to the token in `args->v`.
        *((char **)arena_push(allocator, alignof(char**), sizeof(char**))) = start;
        count++;
        while (end < bound) if (!is_whitespace(*end++))
        {
          start = --end;
          break;
        }
        break;

      default:
        break;
    }
  }
  // Add the last token.
  if (edge_case)
  {
    while (*start++ == '\'');
    start--;

    char *left = start, *right = start, c;
    while (right < end)
      if ((c = *right++) != '\'')
        *left++ = c;

    *left = '\0';
  }
  *((char **)arena_push(allocator, alignof(char**), sizeof(char**))) = start;
  count++;
  // End `args->v` with a null pointer.
  *((char **)arena_push(allocator, alignof(char**), sizeof(char**))) = NULL;
  args->c = count;
  return args;
}

// Removes whitespace (' ', '\n', '\t')
void trim(str* string)
{
  while (string->len)
  {
    if (is_whitespace(*string->data))
    {
      string->data++;
      string->len--;
    } else break;
  }
  int trailing_whitespace = 0;
  while (string->len) {
    if (is_whitespace(*(string->data + string->len - 1)))
    {
      trailing_whitespace = 1;
      string->len--;
    } else break;
  }
  if (trailing_whitespace)
    string->data[string->len] = '\0';
}

int is_whitespace(char c)
{
  return c == ' ' || c == '\n' || c == '\t';
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

void* arena_push(arena *arena, size_t alignment, size_t size)
{
  size_t bit_mask = alignment - 1;
  assert("alignment must be a power of two" && (alignment != 0) && ((alignment & bit_mask) == 0));

  size_t aligned_length = (arena->len + bit_mask) & ~bit_mask;
  assert("arena overflowed" && (arena->capacity >= aligned_length + size));

  arena->len = aligned_length + size;
  return arena->data + aligned_length;
}