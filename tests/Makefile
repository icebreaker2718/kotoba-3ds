# Native build of the plugin core for testing on a PC (no devkitPro needed).
#   make DICT=../dict.bin
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize=shift
CORE := $(wildcard ../plugin/Sources/core/*.cpp)
DICT ?= ../dict.bin
LDLIBS := -lmbedtls -lmbedx509 -lmbedcrypto   # apt install libmbedtls-dev

test_core: test_core.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_core.cpp $(CORE) $(LDLIBS)

test_net: test_net.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_net.cpp $(CORE) $(LDLIBS)

bmp_tool: bmp_tool.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ bmp_tool.cpp $(CORE) $(LDLIBS)

test_wrap: test_wrap.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_wrap.cpp $(CORE) $(LDLIBS)

test_lens: test_lens.cpp $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_lens.cpp $(CORE) $(LDLIBS)

run: test_wrap test_core test_lens
	./test_core $(DICT)
	./test_wrap $(DICT)
	./test_lens $(DICT)

clean:
	rm -f test_core test_net bmp_tool test_wrap test_lens

.PHONY: run clean
