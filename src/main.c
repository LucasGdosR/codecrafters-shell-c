#include <alloca.h>
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

/*=================================================================================================
  STRUCTS
=================================================================================================*/

typedef struct str {
  char *data;
  size_t len;
} str;

/*=================================================================================================
  ENUMS
=================================================================================================*/

enum Builtins {
  Echo,
  Type,
  Exit,
  Builtins_Size,
};

/*=================================================================================================
  GLOBALS
=================================================================================================*/

str PATH;
char *builtins[] = {[Echo]="echo", [Type]="type", [Exit]="exit"};

/*=================================================================================================
  FUNCTIONS
=================================================================================================*/

void handle_type(str *string)
{
  string->data[--string->len] = '\0';

  // Builtin case:
  // For now, all builtins coincidentally have length 4.
  if (string->len == 4)
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
  {
    // PATH is immutable, so make a mutable copy.
    char *path = alloca(PATH.len+1);
    memcpy(path, PATH.data, PATH.len+1);

    char *dir_str;
    while ((dir_str = strsep(&path, PATH_LIST_SEPARATOR)))
    {
      DIR *dir = opendir(dir_str);
      struct dirent *entry;
      if (dir) while ((entry = readdir(dir)))
      {
        if (entry->d_type == DT_REG && strcmp(entry->d_name, string->data) == 0)
        {
          unsigned long path_len = strlen(dir_str);
          char *full_path = alloca(string->len + path_len + 2);
          memcpy(full_path, dir_str, path_len);
          full_path[path_len] = '/';
          memcpy(full_path + path_len + 1, string->data, string->len + 1);
          if (access(full_path, X_OK) == 0)
          {
            printf("%s is %s\n", string->data, full_path);
            return;
          }
        }
      }
      closedir(dir);
    }
  }

  // Default case:
  printf("%s: not found\n", string->data);
}


int main(int argc, char *argv[])
{
  setbuf(stdout, NULL);
  str buffer = {0};
  PATH.data = getenv("PATH");
  PATH.len = strlen(PATH.data);

  // REPL
  for (;;)
  {
    printf("$ ");

    // Read:
    unsigned long len = getline(&buffer.data, &buffer.len, stdin);

    // Eval-Print: builtins:
    // exit
    if (strcmp(buffer.data, "exit\n") == 0)
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
      handle_type(&string);
    }
    // executables or not found
    else
    {
      buffer.data[len - 1] = '\0';

      int executable_found = 0;
      char *path = alloca(PATH.len+1);
      memcpy(path, PATH.data, PATH.len+1);
      char *bp = buffer.data;
      char *executable_name = strsep(&bp, " ");
      unsigned long executable_name_len = strlen(executable_name);

      char *dir_str;
      while ((dir_str = strsep(&path, PATH_LIST_SEPARATOR)) && !executable_found)
      {
        DIR *dir = opendir(dir_str);
        struct dirent *entry;
        if (dir) while ((entry = readdir(dir)))
        {
          if (entry->d_type == DT_REG && strcmp(entry->d_name, executable_name) == 0)
          {
            unsigned long path_len = strlen(dir_str);
            char *full_path = alloca(executable_name_len + path_len + 2);
            memcpy(full_path, dir_str, path_len);
            full_path[path_len] = '/';
            memcpy(full_path + path_len + 1, executable_name, executable_name_len + 1);
            if (access(full_path, X_OK) == 0)
            {
              int rc = fork();
              if (rc == 0)
              {
                // Child process: get args and execute.
                int MAX_ARGS = 64;
                char *args[MAX_ARGS];
                char *arg;
                args[0] = executable_name;
                int i = 1;
                while ((arg = strsep(&bp, " "))) {
                  if (arg[0] != '\0')
                    args[i++] = arg;
                  assert(i < MAX_ARGS);
                }
                args[i] = NULL;
                execv(full_path, args);
              }
              // Original process
              else
              {
                executable_found = 1;
                wait(NULL);
                break;
              }
            }
          }
        }
        closedir(dir);
      }

      if (!executable_found)
        printf("%s: command not found\n", buffer.data);
    }
  }

  return 0;
}
