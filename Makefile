CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -pthread
LDFLAGS  := -ldl -pthread

TARGET     := secure_copy
LIB_TARGET := libcaesar.so

.PHONY: all clean

all: $(LIB_TARGET) $(TARGET)

$(LIB_TARGET): libcaesar.cpp
	$(CXX) -shared -fPIC -o $@ $<

$(TARGET): secure_copy.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(LIB_TARGET) *.txt *.bin *.img log.txt