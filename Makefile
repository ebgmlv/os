CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -pthread
LDFLAGS  := -ldl -pthread

TARGET     := secure_copy
LIB_TARGET := libcaesar.so

.PHONY: all test clean

all: $(LIB_TARGET) $(TARGET)

$(LIB_TARGET): libcaesar.cpp
	$(CXX) -shared -fPIC -o $@ $<

$(TARGET): secure_copy.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test: $(TARGET) $(LIB_TARGET)
	@echo "=== Тест: 3 файла (3 потока) ==="
	@rm -f log.txt
	@echo "AAA" > f1.txt && echo "BBB" > f2.txt && echo "CCC" > f3.txt
	@mkdir -p out_test
	./$(TARGET) f1.txt f2.txt f3.txt out_test 123
	@echo -e "\n=== Содержимое log.txt ==="
	@cat log.txt
	
	@echo -e "\n=== Тест: 5 файлов (очередь) ==="
	@rm -f log.txt
	@echo "1" > f4.txt && echo "2" > f5.txt
	./$(TARGET) f1.txt f2.txt f3.txt f4.txt f5.txt out_test 77
	@echo -e "\n=== Содержимое log.txt ==="
	@cat log.txt

clean:
	rm -f $(TARGET) $(LIB_TARGET) *.txt *.bin 
	rm -rf out_*