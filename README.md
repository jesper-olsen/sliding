# Sliding

This repo holds the unpacked (ctangle'ed) c source for Knuth's [sliding](https://www-cs-faculty.stanford.edu/~knuth/programs/sliding.w) program.

Additionally it adds a web UI for solving 8- and [15-puzzles](https://en.wikipedia.org/wiki/15_puzzle) - a special case of the more general sliding puzzles the program can solve.
The web UI is live [here](jesper-olsen.github.io/sliding/).


## Prerequisites

You will need the following installed:

1. A C compiler (e.g., GCC or Clang)
2. The (emcc)[https://emscripten.org/docs/tools_reference/emcc.html] compiler - for the web UI only.
2. Make 

Clone the repository and build the program:

```bash
git clone https://github.com/jesper-olsen/sliding.git
cd sl-c
make
``` 

## Run

To run the native app on the "silly example" in Knuth's documentation:

```bash
make run
```

To serve the web UI locally
```bash
make serve
```

