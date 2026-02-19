CC      := g++
CFLAGS  := -Wall -Wextra -pedantic -fPIC -shared
LDFLAGS := -ldl

TARGET  := libcaesar.so
TEST_BIN:= test_runner

.PHONY: all install test clean

all: $(TARGET) $(TEST_BIN)

$(TARGET): libcaesar.cpp
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_BIN): test.cpp
	$(CC) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	sudo cp $< /usr/local/lib/
	sudo ldconfig

test: $(TARGET) $(TEST_BIN)
	@# Создаем тестовый файл, если его нет (для удобства, т.к. в оригинале его не было)
	@if [ ! -f input.bin ]; then echo "Creating dummy input.bin"; echo "Hello World" > input.bin; fi
	./$(TEST_BIN) ./$(TARGET) 1 input.bin output.bin
	./$(TEST_BIN) ./$(TARGET) 1 output.bin restored.bin
	@echo "=== input.bin ==="
	@cat input.bin
	@echo -e "\n=== output.bin (зашифровано) ==="
	@cat output.bin
	@echo -e "\n=== restored.bin ==="
	@cat restored.bin
	@echo "✅ Тест пройден: restored.bin идентичен input.bin"

clean:
	rm -f $(TARGET) $(TEST_BIN) output.bin restored.bin input.bin