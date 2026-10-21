# Native build of the plugin core for testing on a PC (no devkitPro needed).
#   make DICT=../dict.bin
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fsanitize=address,undefined
CORE := $(wildcard ../plugin/Sources/core/*.cpp)
DICT ?= ../dict.bin

test_core: test_core.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_core.cpp $(CORE)

test_net: test_net.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_net.cpp $(CORE)

bmp_tool: bmp_tool.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ bmp_tool.cpp $(CORE)

test_wrap: test_wrap.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_wrap.cpp $(CORE)

run: test_wrap test_core
	./test_core $(DICT)
	./test_wrap $(DICT)

clean:
	rm -f test_core test_net bmp_tool test_wrap

.PHONY: run clean
