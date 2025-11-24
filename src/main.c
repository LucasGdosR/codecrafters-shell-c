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
    command[strlen(command) - 1] = '\0';
    printf("%s: command not found\n", command);
  }
  return 0;
}
