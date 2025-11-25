#include <alloca.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  unsigned long len;
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
    char *path = alloca(PATH.len);
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
          char *full_path = alloca(string->len + path_len);
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
  char *buffer;
  PATH.data = getenv("PATH");
  PATH.len = strlen(PATH.data);

  for (;;)
  {
    printf("$ ");
    size_t n = 0;
    getline(&buffer, &n, stdin);
    if (strcmp(buffer, "exit\n") == 0)
      exit(0);
    else if (n >= 5 && strncmp(buffer, "echo ", 5) == 0)
    {
      printf("%s", buffer + 5);
    }
    else if (n >= 5 && strncmp(buffer, "type ", 5) == 0)
    {
      str string = { buffer + 5, n - 5 };
      handle_type(&string);
    }
    else
    {
      buffer[n - 1] = '\0';
      printf("%s: command not found\n", buffer);
    }
  }

  return 0;
}
