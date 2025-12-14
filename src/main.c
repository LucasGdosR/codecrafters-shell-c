#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <readline/readline.h>
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
#define ARENA_PUSH_TYPE(type) ((type*)arena_push(allocator, alignof(type), sizeof(type)))
#define MAX_CWD_SIZE 1024
#define TOKEN_SHIFT 3
#define TOKEN_TYPE_MASK ((1 << TOKEN_SHIFT) - 1)
#define EXTRACT_TOKEN_TYPE(token) (token.t & TOKEN_TYPE_MASK)
#define EXTRACT_TOKEN_PTR(token) ((char*)(token.ptr >> TOKEN_SHIFT))
#define CHAR_PTR_TO_TOKEN(ptr) (((intptr_t) ptr << TOKEN_SHIFT) | Word)

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
  // Redirections:
  RedirectOut,
  RedirectErr,
  AppendOut,
  AppendErr,
  // Multiple commands:
  Pipe,
  Sequential,
  Background,
  Token_Type_Size,
};

static_assert(Token_Type_Size - 1 <= TOKEN_TYPE_MASK, "Token tag does not fit into its mask. Expand shift if possible.");

/*=================================================================================================
  UNIONS
=================================================================================================*/

typedef union token {
  intptr_t ptr; // Shifted by TOKEN_SHIFT to the left
  enum Token_Type t;
} token;
static_assert(sizeof(intptr_t) >= sizeof(void*), "`intptr_t` must fit pointers.");

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
void arena_reset(arena *arena);

char **attempted_completion_function(const char *text, int start, int end);
char *completion_matches_generator(const char *text, int state);

int main(int argc, char *argv[])
{
  setbuf(stdout, NULL);
  PATH.data = getenv("PATH");
  assert(PATH.data && "PATH environment variable not found.");
  PATH.len = strlen(PATH.data);
  arena temp_allocator = {0};
  arena_init(&temp_allocator, ARENA_DEFAULT_SIZE);
  rl_attempted_completion_function = attempted_completion_function;

  // REPL
  char *input;
  while ((input = readline("$ ")))
  {
    ssize_t line_len = strlen(input);
    arena_reset(&temp_allocator);
    char *line_buffer = arena_push(&temp_allocator, alignof(char), line_len + 2); // Double NUL terminator simplifies tokenizing.
    memcpy(line_buffer, input, line_len + 1);
    *(line_buffer + line_len + 1 ) = '\0';
    free(input);

    // Read:
    tokens *tks = tokenize(&temp_allocator);
    commands *cmds = parse(tks, &temp_allocator);
    args *a = cmds->v;

    // Eval-Print:
    for (int i = 0; i < cmds->c; i++)
    {
      if (!a->c) continue;

      // Redirect.
      int saved_fd, target_fd;
      FILE *fp;
      if (a->redirection.t != Word)
      {
        char *modes;
        switch (EXTRACT_TOKEN_TYPE(a->redirection)) {
          case RedirectOut:
            target_fd = STDOUT_FILENO;
            modes = "w";
            break;
          case RedirectErr:
            target_fd = STDERR_FILENO;
            modes = "w";
            break;
          case AppendOut:
            target_fd = STDOUT_FILENO;
            modes = "a";
            break;
          case AppendErr:
            target_fd = STDERR_FILENO;
            modes = "a";
            break;
          case Pipe:
            printf("Pipe redirection not yet implemented.\n");
            break;
          default:
            assert(0 && "Unreachable: invalid redirection.");
            break;
        }
        saved_fd = dup(target_fd);
        assert((saved_fd != -1) && "Failed `dup` for redirection.");
        intptr_t ptr = a->redirection.ptr >> TOKEN_SHIFT;
        char *dst = (char*)ptr;
        enum Token_Type t = a->redirection.t & TOKEN_TYPE_MASK;
        fp = fopen(EXTRACT_TOKEN_PTR(a->redirection), modes);
        assert(fp && "Failed opening file for redirection.");
        int fd = fileno(fp);
        assert((fd != -1) && "Failed `fileno`.");
        int err = dup2(fd, target_fd);
        assert((err != -1) && "Failed `dup2` for starting redirection.");
      }

      // Builtins:
      // TODO (long term): hashtable taking `args.v[0]` as key and function to run as value. If there's no match, run the `executable` branch.
      if (strcmp(a->v[0], builtins[CD]) == 0)
      {
        if ((a->c == 1) || (a->c == 2 && (strcmp(a->v[1], "~")) == 0))
        {
          char *home = getenv("HOME");
          assert(home && "HOME environment variable not found.");
          chdir(home);
        }
        else if (a->c == 2 && chdir(a->v[1]))
          printf("cd: %s: No such file or directory\n", a->v[1]);
        else if (a->c >= 3)
        printf("mysh: cd: too many arguments\n");
      }
      else if (strcmp(a->v[0], builtins[PWD]) == 0)
      {
        char *cwd = arena_push(&temp_allocator, alignof(char), MAX_CWD_SIZE);
        char *ptr = getcwd(cwd, MAX_CWD_SIZE);
        assert(ptr && "`getcwd` failed.");
        printf("%s\n", cwd);
      }
      else if (strcmp(a->v[0], builtins[Exit]) == 0)
        if (a->c == 1)
          exit(0);
        else if (!is_decimal_num(a->v[1]))
          exit(2);
        else if (a->c > 2)
          printf("mysh: exit: too many arguments\n");
        else
          exit((unsigned char) atoll(a->v[1]));
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
        char *full_path = find_executable(
            (str){ a->v[0], strlen(a->v[0]) },
            &temp_allocator);
        if (full_path)
        {
          pid_t pid = fork();
          assert((pid != -1) && "`fork` failed.");
          // Child process
          if (pid == 0)
          {
            execv(full_path, a->v);
            exit(127);
          }
          // Original process
          else
          {
            int wstat;
            pid_t w = waitpid(pid, &wstat, 0);
            assert((w != -1) && "`waitpid` failed.");
          }
        }
        else printf("%s: command not found\n", a->v[0]);
      }
      // Restore redirection.
      if (a->redirection.t != Word)
      {
        dup2(saved_fd, target_fd);
        close(saved_fd);
        fclose(fp);
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
          int err = closedir(dir);
          assert((err != -1) && "Error closing directory.");
          return full_path;
        }
      }
    }
    closedir(dir);
  }
  return NULL;
}

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
    // Multiple commands case
    if (c == '|')
    {
        ARENA_PUSH_TYPE(token)->t = Pipe;
        p++;
    }
    else if (c == '&')
    {
      // Sequential
      if (*(p+1) == '&')
      {
        ARENA_PUSH_TYPE(token)->t = Sequential;
        p += 2;
      }
      else
      {
        ARENA_PUSH_TYPE(token)->t = Background;
        p++;
      }
    }
    // Redirect stdout case
    else if ((c == '>') || ((c == '1') && (*(p+1) == '>') && p++))
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
          do assert(*p && "unterminated single quotes");
          while (*p++ != '\'');
        }
        // Ignore _almost_ everything inside double quotes.
        else if (c == '"')
        {
          should_overwrite = 1;
          while (*p != '"')
          {
            assert(c && "unterminated double quotes");
            char c = *p++;
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
          assert(*p && "escaped NUL terminator");
          p++;
        }
        // Base cases: exit when this token is over.
        else if (!c || is_whitespace(c))
          break;
        // Edge case:
        else if (c == '>' || c == '|' || c == '&')
        {
          assert(0 && "Include whitespaces. Parsing not supported.");
          // These tokens no whitespace require parsing the entire token (> or >>) prior to adding the null terminator, which overwrite the first character.
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
              if (*read == '\\') // ", \, $, `, and newline
              {
                switch (*(read +1))
                {
                  case '"':
                  case '\\':
                  case '$':
                  case '`':
                  case '\n':
                    *write++ = *(read + 1);
                    read += 2;
                    break;
                  default:
                    *write++ = *read++;
                    break;
                }
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
  a->redirection.t = Word;

  int i = 0, argc = 0, cmdc = 0, end = T->c;
  while (i < end) {
    token t = T->v[i++];
    if (EXTRACT_TOKEN_TYPE(t) == Word)
    {
      // Fill `args->v` by pushing words to the arena.
      *ARENA_PUSH_TYPE(char*) = EXTRACT_TOKEN_PTR(t);
      argc++;
    }
    // Redirections:
    else if (t.t <= AppendErr)
    {
      assert(t.t < Token_Type_Size && "This token should not have an embedded pointer.");
      assert(i < end && "Syntax error: redirected without a target.");
      assert(EXTRACT_TOKEN_TYPE(T->v[i]) == Word && "Syntax error: did not redirect to a file.");
      assert(Word == 0 && "This assumes `Word` to be zero. Otherwise, `T->v[i].ptr` would need to be masked.");
      a->redirection.ptr = T->v[i++].ptr | t.t;
    }
    else // Split command
    {
      // FIXME: this does not allow ending a line in & for background tasks.
      assert(argc && "Syntax error: used | or && without a command on left side.");
      a->c = argc;
      if (t.t == Pipe) // Do not add redirection for &&
        a->redirection.t = Pipe; // Pipes don't need a pointer to their destination.
      *ARENA_PUSH_TYPE(char*) = NULL; // Null-terminated `args->v`.
      argc = 0;
      cmdc++;
      a = ARENA_PUSH_TYPE(args);
      a->redirection.t = Word;
    }
  }
  assert(argc > 0 && "Ended a line with a && or | to nowhere.");
  a->c = argc;
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

char **attempted_completion_function(const char *text, int start, int end)
{
  return start ? NULL : rl_completion_matches(text, completion_matches_generator);
}

char *completion_matches_generator(const char *text, int state)
{
  static int i, len;
  if (!state) i = 0, len = strlen(text);
  for (; i < Builtins_Size; i++)
    if (strncmp(builtins[i], text, len) == 0)
      return strdup(builtins[i++]);
  return NULL;
}

void arena_init(arena *arena, size_t size)
{
  arena->data = malloc(size);
  arena->capacity = size;
  if (!arena->data)
  {
    fprintf(stderr, "Failed initializing arena with %zu bytes.\n", size);
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

void arena_reset(arena *arena) { arena->len = 0; }
