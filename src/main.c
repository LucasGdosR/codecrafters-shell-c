#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <readline/readline.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#define ARENA_PUSH_TYPE(arena, type) ((type*)arena_push(arena, alignof(type), sizeof(type)))
#define ALLOCATOR_PUSH_TYPE(type) ARENA_PUSH_TYPE(allocator, type)
#define MAX_CWD_SIZE 1024
#define TOKEN_SHIFT 3
#define TOKEN_TYPE_MASK ((1 << TOKEN_SHIFT) - 1)
#define EXTRACT_TOKEN_TYPE(token) ((token).t & TOKEN_TYPE_MASK)
#define EXTRACT_TOKEN_PTR(token) ((char*)((token).ptr >> TOKEN_SHIFT))
#define CHAR_PTR_TO_TOKEN(ptr) (((intptr_t) (ptr) << TOKEN_SHIFT) | Word)
#define EXTENDED_ASCII 256
#define TRIE_ARRAY_SIZE EXTENDED_ASCII
#define ROUND_UP_INT_DVISION(numer, denom) (((numer) + (denom) - 1) / (denom))
#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LSB64(n) __builtin_ctzll(n)
#define MSB64(n) (63 - __builtin_clzll(n))
#define KB (1 << 10)
#define MB (KB << 10)
#define GB (MB << 10)
#define ALIGN_UP(n, alignment) (((n) + (alignment) - 1) & ~((alignment) - 1))
// +1 for null termination of `args->v`s.
#define ADVANCE_ARGS(a) (args*)((char*)(a) + sizeof(args) + ((a)->c + 1) * sizeof(char*))

/*=================================================================================================
  ENUMS
=================================================================================================*/

enum Builtins {
  CD,
  PWD,
  Echo,
  Type,
  Exit,
  History,
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

/* `token` stores both a pointer and a tag at the SAME TIME. */
typedef union token {
  intptr_t ptr; // Shifted by TOKEN_SHIFT to the left
  enum Token_Type t;
} token;
static_assert(sizeof(intptr_t) >= sizeof(void*), "`intptr_t` must fit pointers.");

/*=================================================================================================
  STRUCTS
=================================================================================================*/

/* String with length. */
typedef struct str {
  char *data;
  size_t len;
} str;

/* Bump allocator. */
typedef struct arena {
  size_t len;
  size_t capacity;
  char *data;
} arena;

/* Growing bump allocator. */
typedef struct arena_exponential {
  size_t len;
  size_t first_block_capacity;
  char *room[6];
} arena_exponential;

/* FAM struct for tokenizing result. */
typedef struct tokens {
  size_t c;
  token v[];
} tokens;

/* FAM struct for args. */
typedef struct args {
  size_t c;
  // Includes both pointer and tag.
  token redirection;
  char *v[];
} args;

/* FAM struct for parsing result. */
typedef struct commands {
  size_t c;
  // Nested FAM struct.
  args v[];
} commands;

/* Cached sorted strings for autocomplete and `type` built-in.
 * Underlying buffer is created by:
 * `malloc(
 *    str_len_sum +               // all executables and full paths
 *    2 * count +                 // null terminator for strings
 *    sizeof(int32_t) * 2 * count // offsets for strings
 *  );`
 * Destroying this is as simple as `free(strings.strings)`
 * and setting fields to 0.
 */
typedef struct permanent_strings {
  // Points to the first executable / built-in string in lexicographic order,
  // followed by all executable strings,
  // followed by all full-paths (same order).
  char *strings;
  // `strings + offsets[n]` finds the n-th executable string.
  // `strings + offsets[count + n]` finds the n-th full-path.
  // `offsets[2 * count]` is out of bounds.
  int32_t *offsets;
  uint32_t count;
} permanent_strings;

typedef struct temp_entry {
  char *name;
  char *full_path; // NULL for built-in
} temp_entry;

/*=================================================================================================
  FUNCTIONS
=================================================================================================*/

static const char* find_executable(const char *restrict target);
static const tokens* tokenize(arena *restrict allocator);
static const commands* parse(const tokens *restrict T, arena *restrict allocator);
static const args* execute_single_command(const args *restrict a, arena *restrict allocator);

static void builtin_cd(const args *restrict a, arena *restrict allocator);
static void builtin_pwd(const args *restrict a, arena *restrict allocator);
static void builtin_echo(const args *restrict a, arena *restrict allocator);
static void builtin_type(const args *restrict a, arena *restrict allocator);
static void builtin_exit(const args *restrict a, arena *restrict allocator);
static void builtin_history(const args *restrict a, arena *restrict allocator);

static int is_whitespace(char c);
static int is_decimal_num(const char *restrict c);
static char* skip_spaces(char *restrict p);

static void arena_init(arena *restrict arena, size_t size);
static void arena_destroy(arena *restrict arena);
static void* arena_push(arena *restrict arena, size_t alignment, size_t size);
static void* arena_exponential_push(arena_exponential *restrict a, size_t alignment, size_t size);
static void arena_reset(arena *restrict arena);

static void build_autocomplete_strings();
static int temp_entry_cmp(const void *a, const void *b);
static int32_t strings_binary_search(const char *restrict target);
static void* init_once(void*);

static char** attempted_completion_function(const char *restrict text, int start, int end);
static char* completion_matches_generator(const char *restrict text, int state);

/*=================================================================================================
  GLOBALS
=================================================================================================*/

/* Mappings from enum to string / functions. */
static const char *builtins[Builtins_Size] = {[CD]="cd", [PWD]="pwd", [Echo]="echo", [Type]="type", [Exit]="exit", [History]="history"};
static void (*const builtin_functions[Builtins_Size])(const args *, arena *) = {
  [CD]=builtin_cd, [PWD]=builtin_pwd, [Echo]=builtin_echo, [Type]=builtin_type, [Exit]=builtin_exit, [History]=builtin_history};
/* Global sorted string list to interface with GNU Readline. */
static permanent_strings strings;
static pthread_once_t strings_once = PTHREAD_ONCE_INIT;

/*=================================================================================================
  IMPLEMENTATIONS
=================================================================================================*/

int main(int argc, char *argv[])
{
  pthread_t tid;
  if (pthread_create(&tid, NULL, init_once, NULL) != 0)
  {
    perror("Error spawning thread. Building string list in main thread instead.\n");
    pthread_once(&strings_once, build_autocomplete_strings);
  }
  pthread_detach(tid);
  setbuf(stdout, NULL);

  arena repl_arena;
  arena_init(&repl_arena, ARENA_DEFAULT_SIZE);

  // GNU Readline interface.
  rl_attempted_completion_function = attempted_completion_function;

  // REPL
  char *input;
  while ((input = readline("$ ")))
  {
    ssize_t line_len = strlen(input);
    arena_reset(&repl_arena);
    // TODO: reuse `readline`'s buffer instead of copying it to arena.
    char *line_buffer = arena_push(&repl_arena, alignof(char), line_len + 2); // Double NUL terminator simplifies tokenizing.
    memcpy(line_buffer, input, line_len + 1);
    *(line_buffer + line_len + 1 ) = '\0';
    free(input);

    // Read:
    const tokens *tks = tokenize(&repl_arena);
    if (tks->c == 0) continue;
    const commands *cmds = parse(tks, &repl_arena);
    const args *a = cmds->v;

    // Eval-Print:
    int i = 0;
    while (i < cmds->c)
    {
      int pipeline_length = 0;
      {
        const args *temp = a;
        while (temp->redirection.t == Pipe)
        {
          temp = ADVANCE_ARGS(temp);
          pipeline_length++;
        }
      }
      if (pipeline_length == 0)
        a = execute_single_command(a, &repl_arena);
      else
      {
        // TODO (long term): figure out how pipes and redirection should interact and implement that.
        int (*pipes)[2] = arena_push(&repl_arena, alignof(int[2]), pipeline_length * sizeof(int[2]));
        for (int i = 0; i < pipeline_length; i++)
          pipe(pipes[i]);
        pid_t *children = arena_push(&repl_arena, alignof(pid_t), (pipeline_length + 1) * sizeof(pid_t));
        for (int i = 0; i <= pipeline_length; i++, a = ADVANCE_ARGS(a))
        {
          pid_t pid = fork();
          assert((pid != -1) && "`fork` failed in pipeline.");
          // Child process
          if (pid == 0)
          {
            // Redirect stdin for all but the first.
            if (i > 0)
              dup2(pipes[i-1][0], STDIN_FILENO);
            // Redirect stdout for all but the last.
            // TODO: check `a->redirection.t` for file redirections?
            if (i < pipeline_length)
              dup2(pipes[i][1], STDOUT_FILENO);

            // Close duplicated pipes.
            for (int i = 0; i < pipeline_length; i++)
            {
              close(pipes[i][0]);
              close(pipes[i][1]);
            }

            // Builtin
            for (int i = 0; i < Builtins_Size; i++) if (strcmp(a->v[0], builtins[i]) == 0)
            {
              builtin_functions[i](a, &repl_arena);
              exit(0);
            }

            // Executable

            const char *full_path = find_executable(a->v[0]);
            if (full_path)
            {
              execv(full_path, a->v);
              exit(EXIT_FAILURE);
            }
            else printf("%s: command not found\n", a->v[0]);
            exit(127); // Command not found exit code.
          }
          // Parent
          children[i] = pid;
        }
        // Close all pipes on parent
        for (int i = 0; i < pipeline_length; i++)
        {
          close(pipes[i][0]);
          close(pipes[i][1]);
        }
        for (int i = 0; i <= pipeline_length; i++)
        {
          int wstat;
          pid_t w = waitpid(children[i], &wstat, 0);
          assert((w != 1) && "`waitpid` failed in pipeline");
        }
      }
      i += pipeline_length + 1;
    }
  }

  return 0;
}

static const args* execute_single_command(const args *restrict a, arena *restrict allocator)
{
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
  int is_builtin = 0;
  for (int i = 0; i < Builtins_Size; i++) if (strcmp(a->v[0], builtins[i]) == 0)
  {
    builtin_functions[i](a, allocator);
    is_builtin = 1;
    break;
  }
  if (!is_builtin) // executable
  {
    const char *full_path = find_executable(a->v[0]);
    if (full_path)
    {
      pid_t pid = fork();
      assert((pid != -1) && "`fork` failed.");
      // Child process
      if (pid == 0)
      {
        execv(full_path, a->v);
        exit(EXIT_FAILURE);
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
  return ADVANCE_ARGS(a);
}

static void builtin_cd(const args *restrict a, arena *restrict allocator)
{
  if ((a->c == 1) || (a->c == 2 && (strcmp(a->v[1], "~")) == 0))
  {
    char *home = getenv("HOME");
    assert(home && "HOME environment variable not found.");
    if (chdir(home))
      printf("cd: %s: No such file or directory\n", a->v[1]);
  }
  else if (a->c == 2 && chdir(a->v[1]))
    printf("cd: %s: No such file or directory\n", a->v[1]);
  else if (a->c >= 3)
  printf("mysh: cd: too many arguments\n");
}

static void builtin_pwd(const args *restrict a, arena *restrict allocator)
{
  char *cwd = arena_push(allocator, alignof(char), MAX_CWD_SIZE);
  char *ptr = getcwd(cwd, MAX_CWD_SIZE);
  assert(ptr && "`getcwd` failed.");
  printf("%s\n", cwd);
}

static void builtin_echo(const args *restrict a, arena *restrict allocator)
{
  size_t end = a->c - 1;
  if (end)
  {
    for (size_t i = 1; i < end; ++i)
      printf("%s ", a->v[i]);
    printf("%s\n", a->v[end]);
  }
}

static void builtin_type(const args *restrict a, arena *restrict allocator)
{
  for (int i = 1; i < a->c; i++)
  {
    char *arg = a->v[i];
    const char *full_path = find_executable(arg);
    if (full_path)
      printf("%s is %s\n", arg, full_path);
    // Default case:
    else printf("%s: not found\n", arg);
  }
}

static void builtin_exit(const args *restrict a, arena *restrict allocator)
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

static void builtin_history(const args *restrict a, arena *restrict allocator)
{

}

static const char* find_executable(const char *restrict target)
{
  int32_t offset = strings_binary_search(target);
  if (offset == -1)
    return NULL;
  char *candidate = strings.strings + strings.offsets[offset];
  return strcmp(target, candidate) == 0 ?
    strings.strings + strings.offsets[strings.count + offset] :
    NULL;
}

static const tokens* tokenize(arena *restrict allocator)
{
  char *p = allocator->data; // User input lies at the start of the arena.

  // Push a slice to the arena. Fill `tokens->v` by pushing tokens on the arena.
  tokens *tks = ALLOCATOR_PUSH_TYPE(tokens);

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
        ALLOCATOR_PUSH_TYPE(token)->t = Pipe;
        p++;
    }
    else if (c == '&')
    {
      // Sequential
      if (*(p+1) == '&')
      {
        ALLOCATOR_PUSH_TYPE(token)->t = Sequential;
        p += 2;
      }
      else
      {
        ALLOCATOR_PUSH_TYPE(token)->t = Background;
        p++;
      }
    }
    // Redirect stdout case
    else if ((c == '>') || ((c == '1') && (*(p+1) == '>') && p++))
    {
      // Append case
      if (*(p+1) == '>')
      {
        ALLOCATOR_PUSH_TYPE(token)->t = AppendOut;
        p += 2;
      }
      else
      {
        ALLOCATOR_PUSH_TYPE(token)->t = RedirectOut;
        p++;
      }
    }
    // Redirect stderr case
    else if ((c == '2') && (*(p+1) == '>'))
    {
      // Append case
      if (*(p+2) == '>')
      {
        ALLOCATOR_PUSH_TYPE(token)->t = AppendErr;
        p += 3;
      }
      else
      {
        ALLOCATOR_PUSH_TYPE(token)->t = RedirectErr;
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
      ALLOCATOR_PUSH_TYPE(token)->ptr = CHAR_PTR_TO_TOKEN(start);
    }
    count++;
  }
  tks->c = count;
  return tks;
}

static const commands *parse(const tokens *restrict T, arena *restrict allocator)
{
  // Push a slice to the arena. Fill `commands->v` by pushing args on the arena.
  commands *cmds = ALLOCATOR_PUSH_TYPE(commands);
  args *a = ALLOCATOR_PUSH_TYPE(args);
  a->redirection.t = Word;

  int i = 0, argc = 0, cmdc = 0, end = T->c;
  while (i < end) {
    token t = T->v[i++];
    if (EXTRACT_TOKEN_TYPE(t) == Word)
    {
      // Fill `args->v` by pushing words to the arena.
      *ALLOCATOR_PUSH_TYPE(char*) = EXTRACT_TOKEN_PTR(t);
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
      *ALLOCATOR_PUSH_TYPE(char*) = NULL; // Null-terminated `args->v`.
      argc = 0;
      cmdc++;
      a = ALLOCATOR_PUSH_TYPE(args);
      a->redirection.t = Word;
    }
  }
  assert(argc > 0 && "Ended a line with a && or | to nowhere.");
  a->c = argc;
  *ALLOCATOR_PUSH_TYPE(char*) = NULL; // Null-terminated `args->v`.
  cmds->c = cmdc + 1;

  return cmds;
}

// Removes whitespace (' ', '\n', '\t')
static char* skip_spaces(char *restrict p)
{
  while (is_whitespace(*p++));
  return p - 1;
}

static int is_whitespace(char c)
{
  return c == ' ' || c == '\n' || c == '\t';
}

static int is_decimal_num(const char *restrict c)
{
  char d;
  while ((d = *c++))
    if (d < '0' || d > '9')
      return 0;
  return 1;
}

// Returns the smallest index into `strings.offsets` such that
// `strings.strings[strings.offsets[idx]]` starts with `target`
// or -1 if no matches.
static int32_t strings_binary_search(const char *restrict target)
{
  pthread_once(&strings_once, build_autocomplete_strings);
  int32_t left = 0, right = strings.count;
  while (left < right)
  {
    int32_t m = (left + right) / 2;
    int cmp = strcmp(target, strings.strings + strings.offsets[m]);
    if (cmp < 0)
      right = m;
    else if (cmp > 0)
      left = m + 1;
    else return m;
  }
  return (left >= strings.count || (strncmp(target, strings.strings + strings.offsets[left], strlen(target)) != 0)) ? -1 : left;
}

static void* init_once(void *_)
{
  pthread_once(&strings_once, build_autocomplete_strings);
  return 0;
}

static void build_autocomplete_strings()
{
  /******************************************************
   * Initialize temporary arenas.
   ******************************************************/
  // Use a scratch arena for `temp_entry` and another for strings.
  arena scratch_entry, scratch_string;
  arena_init(&scratch_entry, 2 * MB);
  arena_init(&scratch_string, 2 * MB);

  /******************************************************
   * Collect strings.
   ******************************************************/
  // Start with built-ins so they have precedence over PATH executables.
  for (int i = 0; i < Builtins_Size; i++)
  {
    temp_entry *e = ARENA_PUSH_TYPE(&scratch_entry, temp_entry);
    size_t len = strlen(builtins[i]) + 1;
    e->name = arena_push(&scratch_string, alignof(char), len);
    memcpy(e->name, builtins[i], len);
    e->full_path = NULL; // builtin marker
  }

  // Executables next.
  char *PATH = getenv("PATH");
  if (PATH)
  {
    // PATH is immutable, so make a mutable copy.
    size_t path_len = strlen(PATH) + 1;
    char *path = arena_push(&scratch_string, alignof(char), path_len);
    memcpy(path, PATH, path_len);

    char *dir;
    while ((dir = strsep(&path, PATH_LIST_SEPARATOR)))
    {
      DIR *d = opendir(dir);
      struct dirent *e;
      if (d)
      {
        size_t dlen = strlen(dir) + 1;
        while ((e = readdir(d)))
        {
          // Skip hidden files.
          if (e->d_name[0] != '.')
          {
            size_t elen = strlen(e->d_name) + 1;
            size_t pop_len = scratch_string.len;
            char *full = arena_push(&scratch_string, alignof(char), elen + dlen);
            memcpy(full, dir, dlen);
            full[dlen - 1] = '/';
            memcpy(full + dlen, e->d_name, elen);

            // Only store executable files.
            struct stat st;
            if (  ( (e->d_type == DT_REG) && (access(full, X_OK) == 0)  ) || // Fast path
             (  (e->d_type == DT_REG && e->d_type == DT_UNKNOWN) && (stat(full, &st) == 0) && (S_ISREG(st.st_mode) ) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH) ) ) )
            {
              temp_entry *entry = ARENA_PUSH_TYPE(&scratch_entry, temp_entry);
              entry->name = arena_push(&scratch_string, alignof(char), elen);
              memcpy(entry->name, e->d_name, elen);
              entry->full_path = full;
            }
            else scratch_string.len = pop_len; // Discard non-executable file paths.
          }
        }
        closedir(d);
      }
    }
  }

  /******************************************************
   * Sort entries by their corresponding strings.
   * Keep it stable using arena addresses.
   ******************************************************/
  // TODO (long term): implement radix sort. This takes 1 ms already, but as practice.
  // Make interface to `qsort()`.
  size_t entry_count = scratch_entry.len / sizeof(temp_entry);
  temp_entry *entries = (temp_entry *)scratch_entry.data;
  qsort(entries, entry_count, sizeof(temp_entry), temp_entry_cmp);

  /******************************************************
   * Deduplicate in place and compute total string sizes.
   ******************************************************/
  temp_entry *read = entries, *write = entries;
  size_t names_len_sum = 0, paths_len_sum = 0;
  for (size_t i = 0; i < entry_count; i++, read++)
    if (write == entries || (strcmp(read->name, (write - 1)->name) != 0))
    {
      *write++ = *read;
      names_len_sum += strlen(read->name);
      if (read->full_path) // Skip built-ins
        paths_len_sum += strlen(read->full_path);
    }
  size_t count = write - entries;

  /******************************************************
   * Allocate permanent block.
   ******************************************************/
  const char *builtin_msg = "a shell builtin";
  size_t builtin_msg_len = strlen(builtin_msg) + 1;
  size_t strings_bytes = names_len_sum + count + builtin_msg_len + paths_len_sum + (count - Builtins_Size);
  size_t aligned_length = ALIGN_UP(strings_bytes, alignof(int32_t));
  size_t block_size = aligned_length + sizeof(int32_t) * 2 * count;
  char *block = malloc(block_size);
  assert(block && "malloc failed ¯\\_(ツ)_/¯");

  strings.strings = block;
  strings.offsets = (int32_t*)(block + aligned_length);
  strings.count = count;

  /******************************************************
   * Store sorted strings and offsets.
   ******************************************************/
  // Memory layout:
  // [strings(exe/builtins)]["a shell builtin"][strings(exe_full)][idx into part 1][idx into part 2/3]
  // 5 pointers to fill / track.
  char *names = block;
  char *builtin_msg_ptr = block + names_len_sum + count;
  // Done filling / tracking 1/5
  memcpy(builtin_msg_ptr, builtin_msg, builtin_msg_len);
  char *paths = builtin_msg_ptr + builtin_msg_len;
  int32_t *name_offsets = strings.offsets;
  int32_t *path_offsets = strings.offsets + count;

  // i tracks index into offsets, done tracking 3/5
  for (size_t i = 0; i < count; i++)
  {
    temp_entry e = entries[i];
    // Done filling 2/5
    name_offsets[i] = names - block;
    size_t nlen = strlen(e.name) + 1;
    // Done filling 3/5
    memcpy(names, e.name, nlen);
    // Done tracking 4/5
    names += nlen;

    // Builtin flag
    if (e.full_path)
    {
      // Must also fill in the other branch!
      path_offsets[i] = paths - block;
      size_t plen = strlen(e.full_path) + 1;
      // Done filling 4/5
      memcpy(paths, e.full_path, plen);
      // Done tracking 5/5
      paths += plen;
    }
    // Done filling 5/5
    else path_offsets[i] = builtin_msg_ptr - block;
  }

  /******************************************************
   * Memory cleanup.
   ******************************************************/
  arena_destroy(&scratch_entry);
  arena_destroy(&scratch_string);
}

static int temp_entry_cmp(const void *a, const void *b)
{
  const temp_entry *ea = a;
  const temp_entry *eb = b;
  int cmp = strcmp(ea->name, eb->name);
  // Address implies insertion order in arena.
  return cmp ? cmp : ea < eb ? -1 : ea > eb;
};

static char **attempted_completion_function(const char *restrict text, int start, int end)
{
  return start ? NULL : rl_completion_matches(text, completion_matches_generator);
}

static char *completion_matches_generator(const char *restrict text, int state)
{
  static int32_t idx, len;
  if (!state)
  {
    if ((idx = strings_binary_search(text)) == -1)
      return NULL;
    else
      len = strlen(text);
  }
  char *candidate = strings.strings + strings.offsets[idx++];
  if (idx <= strings.count && strncmp(text, candidate, len) == 0)
    return strdup(candidate);
  return NULL;
}

static void arena_init(arena *restrict arena, size_t size)
{
  arena->data = malloc(size);
  arena->capacity = size;
  arena->len = 0;
  if (!arena->data)
  {
    fprintf(stderr, "Failed initializing arena with %zu bytes.\n", size);
    exit(1);
  }
}

static void arena_destroy(arena *restrict arena) { free(arena->data); }

static void* arena_push(arena *restrict arena, size_t alignment, size_t size)
{
  size_t bit_mask = alignment - 1;
  assert((alignment != 0) && ((alignment & bit_mask) == 0) && "alignment must be a power of two");

  size_t aligned_length = (arena->len + bit_mask) & ~bit_mask;
  assert((arena->capacity >= aligned_length + size) && "arena overflowed");

  arena->len = aligned_length + size;
  return arena->data + aligned_length;
}

static void* arena_exponential_push(arena_exponential *restrict arena, size_t alignment, size_t size)
{
  // Get the MSB of `arena->len / arena->first_block_capacity`.
  int block_idx = MSB64(arena->len / arena->first_block_capacity);
  /****************************
   * len / capacity cases:
   * 0 => first block
   * 1 => second block => 2x capacity
   * 2-3 => third block => 4x capacity
   * 4-7 => fourth block => 8x capacity
   * 8-15 => fifth block => 16x capacity
   * 16-31 => sixth block => 32x capacity
   ****************************/
  int capacity = arena->first_block_capacity << block_idx;

  size_t bit_mask = alignment - 1;
  assert((alignment != 0) && ((alignment & bit_mask) == 0) && "alignment must be a power of two");

  size_t aligned_length = (arena->len + bit_mask) & ~bit_mask;
  // No room left in this block. Skip to the next
  while (aligned_length + size > capacity)
  {
    assert(block_idx <= 4 && "arena_exponential overflowed");
    block_idx++;
    aligned_length = capacity;
    arena->room[block_idx] = malloc(capacity);
    capacity <<= 1;
  }

  arena->len = aligned_length + size;

  return arena->room[block_idx] + aligned_length - (block_idx ? capacity >> 1 : 0);
}

static void arena_reset(arena *restrict arena) { arena->len = 0; }
