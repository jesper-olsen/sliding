CC = clang
CFLAGS = -Wall -O3 -std=c23 -ffast-math -march=native -DNDEBUG
#CFLAGS = -Wall -O0 -std=c23 -g -fsanitize=address -fsanitize=thread
LDFLAGS = -lm

GB_FLIP_TEST  := gb_flip.c test_flip.c
SLIDING_SRC   := gb_flip.c sliding.c
API_TEST      := gb_flip.c sliding.c test_api.c
HEADERS       := gb_flip.h sliding.h

# --- wasm ------------------------------------------------------------------
# MEMBITS sizes the configuration arena (2^24 words = 64MB, good for about
# depth 20 on a 4x4). HASHBITS sizes the hash table (2^21 entries = 16MB).
EMCC       ?= emcc
EM_TUNING  := -DMEMBITS=24 -DHASHBITS=21
EM_EXPORTS := '["_sliding_solve","_sliding_moves","_sliding_rows","_sliding_cols","_sliding_path","_sliding_message","_malloc","_free"]'
EMFLAGS    := -O3 -std=c23 $(EM_TUNING) \
              -sMODULARIZE=1 \
              -sEXPORT_NAME=createSlidingModule \
              -sENVIRONMENT=web \
              -sINVOKE_RUN=0 \
              -sINITIAL_MEMORY=134217728 \
              -sALLOW_MEMORY_GROWTH=1 \
              -sSTACK_SIZE=1048576 \
              -sEXPORTED_FUNCTIONS=$(EM_EXPORTS) \
              -sEXPORTED_RUNTIME_METHODS='["ccall","HEAPU8"]'

ASTYLE_OPTS := --style=kr \
               --indent=spaces=8 \
               --pad-oper \
               --pad-comma \
               --unpad-brackets \
               --squeeze-ws

.PHONY: all clean fmt run test wasm serve

all: gb_flip sliding

gb_flip: $(GB_FLIP_TEST) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(GB_FLIP_TEST) $(LDFLAGS)

sliding: $(SLIDING_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SLIDING_SRC) $(LDFLAGS)

test_api: $(API_TEST) $(HEADERS)
	$(CC) $(CFLAGS) -DSLIDING_NO_MAIN -o $@ $(API_TEST) $(LDFLAGS)

wasm: sliding.js

sliding.js: $(SLIDING_SRC) $(HEADERS)
	$(EMCC) $(SLIDING_SRC) -o $@ $(EMFLAGS)

serve: sliding.js
	python3 -m http.server 8000

clean:
	rm -f gb_flip sliding test_api sliding.js sliding.wasm

fmt:
	astyle $(ASTYLE_OPTS) $(GB_FLIP_TEST) $(SLIDING_SRC) $(HEADERS)

run: sliding
	./sliding 0 < example_silly.txt

test: sliding test_api
	./test_api example_silly.txt 0
	./test_api example_puzzle8.txt 0 1
