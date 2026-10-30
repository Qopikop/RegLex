# Compiler workbench build.   make            -> build/rcc, ./lexer
#                             make test       -> unit, differential and native-runtime tests
#                             make sanitize   -> build/rcc-asan with AddressSanitizer + UBSan
CC      ?= cc
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDFLAGS ?= -lm -lpthread

LEX_SRC  = src/util.c src/regex.c src/automata.c src/lexer.c
ALL_SRC  = $(LEX_SRC) src/ast.c src/parse.c src/parse_c.c src/parse_py.c src/ir.c src/value.c src/lower.c src/opt.c src/interp.c
HEADERS  = $(wildcard src/*.h)

all: build/rcc lexer build/test_automata

build/general_rules.h: rules/general.rules
	@mkdir -p build
	cd rules && xxd -i general.rules > ../build/general_rules.h.tmp
	sed 's/general_rules/rules_general_rules/g' build/general_rules.h.tmp > $@ && rm build/general_rules.h.tmp

build/rcc: $(ALL_SRC) src/rcc.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(ALL_SRC) src/rcc.c $(LDFLAGS)

lexer: main.c $(LEX_SRC) $(HEADERS) build/general_rules.h
	$(CC) $(CFLAGS) -o $@ main.c $(LEX_SRC) $(LDFLAGS)

build/test_automata: tests/test_automata.c $(LEX_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_automata.c $(LEX_SRC) $(LDFLAGS)

sanitize: $(ALL_SRC) src/rcc.c $(HEADERS)
	@mkdir -p build
	$(CC) -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-omit-frame-pointer -o build/rcc-asan $(ALL_SRC) src/rcc.c $(LDFLAGS)

test: all
	./build/test_automata
	python3 tests/run_tests.py

clean:
	rm -rf build lexer

.PHONY: all test sanitize clean
