BUILD := build
COV   := build-coverage
GCOV  ?= gcov

.PHONY: test coverage clean

test:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD) -j
	./$(BUILD)/order_book_test

coverage:
	cmake -S . -B $(COV) -DCOVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(COV) -j
	find $(COV) -name '*.gcda' -delete
	./$(COV)/order_book_test --gtest_brief=1
	gcovr --gcov-executable "$(GCOV)" --root . --filter 'order_book\.(h|cpp)$$' --filter 'order\.h$$' \
	      --object-directory $(COV) --print-summary --txt --fail-under-line 80
	gcovr --gcov-executable "$(GCOV)" --root . --filter 'order_book\.(h|cpp)$$' --filter 'order\.h$$' \
	      --object-directory $(COV) --html-details $(COV)/coverage.html

clean:
	rm -rf $(BUILD) $(COV)
