CC = clang
CFLAGS = -Wall -O3 -std=c23 -ffast-math -march=native -DNDEBUG
#CFLAGS = -Wall -O0 -std=c23 -g -fsanitize=address -fsanitize=thread
LDFLAGS = -lm

GB_FLIP_TEST  := gb_flip.c test_flip.c
SLIDING_SRC   := gb_flip.c sliding.c
HEADERS       := gb_flip.h

//ASTYLE_OPTS := --style=1tbs \
ASTYLE_OPTS := --style=kr \
               --indent=spaces=8 \
               --pad-oper \
               --pad-comma \
               --unpad-brackets \
               --squeeze-ws

.PHONY: all clean fmt

all: gb_flip sliding

gb_flip: $(GB_FLIP_TEST) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(GB_FLIP_TEST) $(LDFLAGS)

sliding: $(SLIDING_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SLIDING_SRC) $(LDFLAGS)

clean:
	rm -f gb_flip sliding

fmt:
	astyle $(ASTYLE_OPTS) $(GB_FLIP_TEST) $(SLIDING_SRC) $(HEADERS)

run:
	./sliding 0 < silly_example.txt
