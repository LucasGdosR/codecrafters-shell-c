# LUSH: Lucas' Shell

LUSH is my take on CodeCrafters' ["Build Your Own Shell" Challenge](https://app.codecrafters.io/courses/shell/overview). Currently 43 / 43 stages complete, waiting for the next extension.

> In this challenge, you'll build your own POSIX compliant shell that's capable of interpreting shell commands, running external programs and builtin commands like cd, pwd, echo and more. Along the way, you'll learn about shell command parsing, REPLs, builtin commands, and more.

## Functionalities

- Built-ins: `echo`, `exit`, `type`, `pwd`, `cd`;
- Runs executables found in PATH;
- Redirects stdout / stderr to files, supporting both truncate and append modes;
- File, built-ins and executables autocomplete w/ TAB using GNU Readline;
- Sequential commands with && in a single line;
- Pipes;
- History;

## Highlights

- Destructive parsing, reusing the input buffer and overwriting token boundaries with null terminators (TODO: get rid of `memcopy` from Readline);
- Tokens stored in 8 bytes, storing both a pointer shifted to the left and a tag in the least significant bits;
- Flexible Array Members (FAM) that store pointers directly into the input buffer that was modified;
- Arena memory management, including exponential growing arena with bit hacks;
- Sorted string list implementation for fast autocomplete with low memory overhead (history has a trie implementation with higher memory overhead);
- Does initialization in a background thread to let the user type right away;

**Note**: Head over to [codecrafters.io](https://codecrafters.io) to try the challenge.
