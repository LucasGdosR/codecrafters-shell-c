#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *builtins[3] = {"echo", "type", "exit"};

void handle_type(char *line, int len) {
  line[len - 1] = '\0';
  if (len == 5) {
    for (int i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    {
      if (strcmp(line, builtins[i]) == 0) {
        printf("%s is a shell builtin\n", line);
        return;
      }
    }
  }
  printf("%s: not found\n", line);
}

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);
  char command[1024];

  for (;;)
  {
    printf("$ ");
    fgets(command, 1024, stdin);
    int len = strlen(command);
    if (strcmp(command, "exit\n") == 0) exit(0);
    else if (len >= 5 && strncmp(command, "echo ", 5) == 0) {
      printf("%s", command + 5);
    } else if (len >= 5 && strncmp(command, "type ", 5) == 0) {
      handle_type(command + 5, len - 5);
    } else {
      command[len - 1] = '\0';
      printf("%s: command not found\n", command);
    }
  }
  return 0;
}
