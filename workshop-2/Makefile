CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: test sanitize clean

test: mem_pool_test
	./mem_pool_test

mem_pool_test: mem_pool_test.cpp mem_pool.h
	$(CXX) $(CXXFLAGS) mem_pool_test.cpp -o $@

sanitize:
	$(CXX) -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer mem_pool_test.cpp -o mem_pool_test_san && ./mem_pool_test_san

clean:
	rm -f mem_pool_test mem_pool_test_san
