#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    } else {
      command[len - 1] = '\0';
      printf("%s: command not found\n", command);
    }
  }
  return 0;
}
