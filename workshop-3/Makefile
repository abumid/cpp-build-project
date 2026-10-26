CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
HEADERS  := mem_pool.h id_map.h order.h order_book.h

.PHONY: test bench sanitize clean

test: tests
	./tests

tests: tests.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) tests.cpp -o $@

bench: bench.cpp $(HEADERS)
	$(CXX) -std=c++20 -O3 -march=native -DNDEBUG bench.cpp -o bench_bin && ./bench_bin

sanitize: tests.cpp $(HEADERS)
	$(CXX) -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests.cpp -o tests_san && ./tests_san

clean:
	rm -f tests tests_san bench_bin
