#include <alloca.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define PATH_LIST_SEPARATOR ";"
#else
#define PATH_LIST_SEPARATOR ":"
#endif

typedef struct str {
  char *data;
  unsigned long len;
} str;

str PATH;
char *builtins[3] = {"echo", "type", "exit"};

void handle_type(str *string) {
  string->data[--string->len] = '\0';

  // Builtin case:
  if (string->len == 4) {
    for (int i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    {
      if (strcmp(string->data, builtins[i]) == 0) {
        printf("%s is a shell builtin\n", string->data);
        return;
      }
    }
  }

  // Executable case:
  {
    char *path = alloca(PATH.len);
    strcpy(path, PATH.data);
    char *dir_str;
    while ((dir_str = strsep(&path, PATH_LIST_SEPARATOR)))
    {
      DIR *dir = opendir(dir_str);
      struct dirent *entry;
      if (dir) while ((entry = readdir(dir))) {
        if (entry->d_type == DT_REG && strcmp(entry->d_name, string->data) == 0) {
          unsigned long path_len = strlen(dir_str);
          char *full_path = alloca(string->len + path_len);
          strcpy(full_path, dir_str);
          full_path[path_len] = '/';
          strcpy(full_path + path_len + 1, string->data);
          if (access(full_path, X_OK) == 0) {
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

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  char command[1024];
  PATH.data = getenv("PATH");
  PATH.len = strlen(PATH.data);
  for (;;)
  {
    printf("$ ");
    fgets(command, 1024, stdin);
    int len = strlen(command);
    if (strcmp(command, "exit\n") == 0) exit(0);
    else if (len >= 5 && strncmp(command, "echo ", 5) == 0) {
      printf("%s", command + 5);
    } else if (len >= 5 && strncmp(command, "type ", 5) == 0) {
      str string = { command + 5, len - 5 };
      handle_type(&string);
    } else {
      command[len - 1] = '\0';
      printf("%s: command not found\n", command);
    }
  }
  return 0;
}
