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
	@echo "=== Тест: 3 файла (SEQUENTIAL по авто-выбору) ==="
	@rm -f log.txt
	@echo "AAA" > f1.txt && echo "BBB" > f2.txt && echo "CCC" > f3.txt
	@mkdir -p out_test
	./$(TARGET) f1.txt f2.txt f3.txt out_test 123
	@echo -e "\n=== Тест: 10 файлов (PARALLEL по авто-выбору) ==="
	@rm -f log.txt
	@for i in $$(seq 1 10); do echo "Content $$i" > f$$i.txt; done
	./$(TARGET) f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt f7.txt f8.txt f9.txt f10.txt out_test 77
	@echo -e "\n=== Тест: явное указание режима ==="
	./$(TARGET) --mode=sequential f1.txt f2.txt out_test 42
	@echo -e "\n=== Лог ==="
	@tail -20 log.txt

clean:
	rm -f $(TARGET) $(LIB_TARGET) *.txt *.bin 
	rm -rf out_*