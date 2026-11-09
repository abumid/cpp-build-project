BUILD := build
COV   := build-coverage
GCOV  ?= gcov

.PHONY: test bench coverage clean

test:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j
	./$(BUILD)/order_book_test

bench:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j
	./$(BUILD)/order_book_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
	    --benchmark_out=bench_results.json --benchmark_out_format=json

coverage:
	cmake -S . -B $(COV) -DCOVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(COV) -j --target order_book_test
	find $(COV) -name '*.gcda' -delete
	./$(COV)/order_book_test --gtest_brief=1
	gcovr --gcov-executable "$(GCOV)" --root . --filter '(tick|map)_order_book\.(h|cpp)$$' --filter 'book_types\.(h|cpp)$$' \
	      --object-directory $(COV) --print-summary --txt --fail-under-line 80

clean:
	rm -rf $(BUILD) $(COV)
