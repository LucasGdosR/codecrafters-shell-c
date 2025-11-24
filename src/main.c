#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);

  printf("$ ");
  char command[1024];
  fgets(command, 1024, stdin);
  command[strlen(command) - 1] = '\0';
  printf("%s: command not found", command);
  return 0;
}
