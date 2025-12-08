#include <assert.h>
#include <dirent.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
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
#define TOKEN_SHIFT 3
#define TOKEN_MASK ((1 << TOKEN_SHIFT) - 1)
#define ARENA_PUSH_TYPE(type) ((type*)arena_push(allocator, alignof(type), sizeof(type)))
#define EXTRACT_TOKEN_PTR(token) ((char*)(token.ptr >> TOKEN_SHIFT))
#define CHAR_PTR_TO_TOKEN(ptr) (((int64_t) ptr << TOKEN_SHIFT) | Word)

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

enum Token_Type {
  Word,
  RedirectOut,
  RedirectErr,
  AppendOut,
  AppendErr,
  Token_Type_Size,
};

static_assert(Token_Type_Size - 1 <= TOKEN_MASK, "Token tag does not fit into its mask. Expand shift if possible.");

/*=================================================================================================
  UNIONS
=================================================================================================*/

typedef union token {
  int64_t ptr; // Shifted by TOKEN_SHIFT to the left
  enum Token_Type t;
} token;

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

typedef struct tokens {
  size_t c;
  token v[];
} tokens;

typedef struct args {
  size_t c;
  token redirection;
  char *v[];
} args;

typedef struct commands {
  size_t c;
  args v[];
} commands;

/*=================================================================================================
  GLOBALS
=================================================================================================*/

str PATH;
char *builtins[] = {[CD]="cd", [PWD]="pwd", [Echo]="echo", [Type]="type", [Exit]="exit"};

/*=================================================================================================
  FUNCTIONS
=================================================================================================*/

char* find_executable(str target, arena *allocator);
tokens* tokenize(arena *allocator);
commands *parse(tokens* T, arena *allocator);
void type_builtin(char *arg, arena *allocator);

int is_whitespace(char c);
int is_decimal_num(char *c);
char* skip_spaces(char *p);

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
    // This resets the arena by setting `allocator->len`.
    temp_allocator.len = getline(&temp_allocator.data, &temp_allocator.capacity, stdin);
    if (temp_allocator.len == -1) exit(0); // EOF
    tokens *tks = tokenize(&temp_allocator);
    commands *cmds = parse(tks, &temp_allocator);
    args *a = cmds->v;

    // Eval-Print:
    for (int i = 0; i < cmds->c; i++) {
      /*for (int i = 0; i < a->c; i++) {
        printf("arg: %s ", a->v[i]);
      }
      printf("redirect %u\n", a->redirection.t);*/
      // TODO: Implement redirection.
      if (!a->c) continue;

      // Builtins:
      // TODO (long term): hashtable taking `args.v[0]` as key and function to run as value. If there's no match, run the `executable` branch.
      if (strcmp(a->v[0], builtins[CD]) == 0)
      {
        if ((a->c == 1) || (a->c == 2 && (strcmp(a->v[1], "~")) == 0))
          chdir(getenv("HOME"));
        else if (a->c == 2 && chdir(a->v[1]))
          printf("cd: %s: No such file or directory\n", a->v[1]);
        else printf("mysh: cd: too many arguments\n");
      }
      else if (strcmp(a->v[0], builtins[PWD]) == 0)
      {
        char *cwd = arena_push(&temp_allocator, alignof(char), MAX_CWD_SIZE);
        getcwd(cwd, MAX_CWD_SIZE);
        printf("%s\n", cwd);
      }
      else if (strcmp(a->v[0], builtins[Exit]) == 0)
      {
        if (a->c == 1)
          exit(0);
        else if (!is_decimal_num(a->v[1]))
          exit(2);
        else if (a->c > 2)
          printf("mysh: exit: too many arguments\n");
        else
          exit((unsigned char) atoll(a->v[1]));
      }
      else if (strcmp(a->v[0], builtins[Echo]) == 0)
      {
        size_t end = a->c - 1;
        if (end)
        {
          for (size_t i = 1; i < end; ++i)
            printf("%s ", a->v[i]);
          printf("%s\n", a->v[end]);
        }
      }
      else if (strcmp(a->v[0], builtins[Type]) == 0)
        for (int i = 1; i < a->c; i++)
          type_builtin(a->v[i], &temp_allocator);
      else // executable
      {
        // TODO: debug this path. Some kind of process spawning is happening wrong. Multiple exits are required after invoking an executable, and they do not execute.
        char *full_path = find_executable(
            (str){ a->v[0], strlen(a->v[0]) },
            &temp_allocator);
        if (full_path) {
          // Child process: get args and execute.
          if (fork() == 0) execv(full_path, a->v);
          // Original process
          else wait(NULL);
        }
        else printf("%s: command not found\n", a->v[0]);
      }
      // Advance args pointer to the next args.
      a = (args*)((char*)a + sizeof(args) + (a->c + 1) * sizeof(char*)); // +1 for null termination of `args->v`s.
    }
  }

  return 0;
}

void type_builtin(char *arg, arena *allocator)
{
  size_t len = strlen(arg);
  // Builtin case:
  // For now, all builtins have length <= 4.
  // TODO (long term): once we have a hashtable with functions, check in that instead of iterating over an array.
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

// TODO (long term): this needlessly iterates over directory entries. Just try `access` concatenating `dir_str` and `target`.
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

/*
  TODO (short term): https://claude.ai/chat/f6236558-c8a3-45a3-aa51-4933f1f28070
  Build commands out of tokens. Each command has args.c and args.v, which is terminated by a null pointer.
  A redirection or pipe token splits commands.

  Planned memory layout:
  [input]
  [tokens [count (reserve space, fill when known)] [v FAM [token [token_kind_enum] [char *start (null if `>`, `>>`, `|`, etc) pointing into `input`] ] ...] ]
  [commands [count (reserve space, fill when known)] [v FAM [args [argc] [argv] ]
  Commands has a count of commands, each command has a count of args. Commands length = for every command, sum 8 (8 bytes argc) + 8 times count (8 bytes per argv char *) + 8 (null pointer).
  We can optimize `tokens` by storing the token_kind_enum in the LSB of the pointer, and masking the pointer when we need to dereference it.

*/
tokens* tokenize(arena *allocator)
{
  char *p = allocator->data; // User input lies at the start of the arena.

  // Push a slice to the arena. Fill `tokens->v` by pushing tokens on the arena.
  tokens *tks = ARENA_PUSH_TYPE(tokens);

  size_t count = 0; // Keep count of tokens for `tokens->c`.
  while (*p)
  {
    p = skip_spaces(p);
    char c = *p;
    // Finished parsing
    if (!c)
      break;
    // Redirect stdout case
    if ((c == '>') || ((c == '1') && (*(p+1) == '>') && p++))
    {
      // Append case
      if (*(p+1) == '>')
      {
        ARENA_PUSH_TYPE(token)->t = AppendOut;
        p += 2;
      }
      else
      {
        ARENA_PUSH_TYPE(token)->t = RedirectOut;
        p++;
      }
    }
    // Redirect stderr case
    else if ((c == '2') && (*(p+1) == '>'))
    {
      // Append case
      if (*(p+2) == '>')
      {
        ARENA_PUSH_TYPE(token)->t = AppendErr;
        p += 3;
      }
      else
      {
        ARENA_PUSH_TYPE(token)->t = RedirectErr;
        p += 2;
      }
    }
    // Word case
    else
    {
      char *start = p;
      int should_overwrite = 0;

      for (;;)
      {
        char c = *p++;
        // Ignore everything inside single quotes.
        if (c == '\'')
        {
          should_overwrite = 1;
          while (*p++ != '\'')
            assert(*p && "unterminated single quotes");
        }
        // Ignore _almost_ everything inside double quotes.
        else if (c == '"')
        {
          should_overwrite = 1;
          while (*p != '"')
          {
            char c = *p++;
            assert(c && "unterminated double quotes");
            if (c == '\\')
            {
              p++;
              assert(*p && "unterminated double quotes");
            }
          }
          p++;
        }
        // Escape any character.
        else if (c == '\\')
        {
          should_overwrite = 1;
          p++;
        }
        // Base cases: exit when this token is over.
        else if (!c || is_whitespace(c))
          break;
        // Edge case:
        else if (c == '>') // Also applies to pipes
        {
          // Redirection with no whitespace requires parsing the redirection (> or >>) prior to adding the null terminator, which might overwrite the '>'.
          // Edge case: what happens when a word is next to `>`? We need to null terminate the word to point into it, but we can't simply overwrite `>` and skip it. Lookahead.
          // TODO: implement this.
          break;
        }
        // Default: get next character.
      }

      // Add token to list.
      if (!should_overwrite)
        *(p - 1) = '\0';
      else
      {
        char *read = start, *write = start;
        while (read < p)
        {
          char c = *read++;
          if (c == '\'')
          {
            while (*read != '\'')
              *write++ = *read++;
            read++;
          }
          else if (c == '\\')
            *write++ = *read++;
          else if (c == '"')
          {
            while (*read != '"')
              if (*read == '\\')
              {
                *write++ = *(read + 1);
                read += 2;
              }
              else *write++ = *read++;
            read++;
          }
          else *write++ = c;
        }
        *(write - 1) = '\0';
      }
      ARENA_PUSH_TYPE(token)->ptr = CHAR_PTR_TO_TOKEN(start);
    }
    count++;
  }
  tks->c = count;
  return tks;
}

commands *parse(tokens* T, arena *allocator)
{
  // Push a slice to the arena. Fill `commands->v` by pushing args on the arena.
  commands *cmds = ARENA_PUSH_TYPE(commands);
  args *a = ARENA_PUSH_TYPE(args);

  /*for (int i = 0; i < T->c; i++)
  {
    if ((T->v[i].t & TOKEN_MASK) == Word)
      printf("token: %s\n", EXTRACT_TOKEN_PTR(T->v[i]));
    else printf("token: redir %u\n", T->v[i].t);
  }*/

  int i = 0, argc = 0, cmdc = 0, end = T->c;
  while (i < end) {
    token t = T->v[i++];
    if ((t.t & TOKEN_MASK) == Word)
    {
      // Fill `args->v` by pushing words to the arena.
      *ARENA_PUSH_TYPE(char*) = EXTRACT_TOKEN_PTR(t);
      argc++;
    }
    else
    {
      assert(argc && "Redirected nothing or two redirections in a row.");
      a->c = argc;
      a->redirection.t = t.t;
      *ARENA_PUSH_TYPE(char*) = NULL; // Null-terminated `args->v`.
      argc = 0;
      cmdc++;
      a = ARENA_PUSH_TYPE(args);
    }
  }
  assert(argc > 0 && "Ended a line with a redirection to nowhere.");
  a->c = argc;
  a->redirection.t = Word;
  *ARENA_PUSH_TYPE(char*) = NULL; // Null-terminated `args->v`.
  cmds->c = cmdc + 1;

  return cmds;
}

// Removes whitespace (' ', '\n', '\t')
char* skip_spaces(char *p)
{
  while (is_whitespace(*p++));
  return p - 1;
}

int is_whitespace(char c)
{
  return c == ' ' || c == '\n' || c == '\t';
}

int is_decimal_num(char *c)
{
  char d;
  while ((d = *c++))
    if (d < '0' || d > '9')
      return 0;
  return 1;
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
  assert((alignment != 0) && ((alignment & bit_mask) == 0) && "alignment must be a power of two");

  size_t aligned_length = (arena->len + bit_mask) & ~bit_mask;
  assert((arena->capacity >= aligned_length + size) && "arena overflowed");

  arena->len = aligned_length + size;
  return arena->data + aligned_length;
}