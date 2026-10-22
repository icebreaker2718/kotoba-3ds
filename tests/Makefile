# Native build of the plugin core for testing on a PC (no devkitPro needed).
#   make DICT=../dict.bin
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize=shift
CORE := $(wildcard ../plugin/Sources/core/*.cpp)
DICT ?= ../dict.bin
LDLIBS := libvocr.a -lmbedtls -lmbedx509 -lmbedcrypto -lm   # apt install libmbedtls-dev
CFLAGS_VOCR := -std=c99 -O2 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize=shift
CXXFLAGS += -I../plugin/Includes/vocr
VOCR_SRC := $(wildcard ../plugin/Sources/vocr/*.c)

# the OCR runtime (C), linked into every test
libvocr.a: $(VOCR_SRC) $(wildcard ../plugin/Includes/vocr/*.h)
	rm -f $@ vocr_*.o
	for f in $(VOCR_SRC); do $(CC) $(CFLAGS_VOCR) -I../plugin/Includes/vocr -c $$f -o vocr_$$(basename $$f .c).o || exit 1; done
	ar rcs $@ vocr_*.o

test_core: test_core.cpp libvocr.a $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_core.cpp $(CORE) $(LDLIBS)

test_net: test_net.cpp libvocr.a $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_net.cpp $(CORE) $(LDLIBS)

bmp_tool: bmp_tool.cpp libvocr.a $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ bmp_tool.cpp $(CORE) $(LDLIBS)

test_wrap: test_wrap.cpp libvocr.a $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_wrap.cpp $(CORE) $(LDLIBS)

test_lens: test_lens.cpp libvocr.a $(CORE) $(wildcard ../plugin/Includes/core/*.hpp)
	$(CXX) $(CXXFLAGS) -I../plugin/Includes -o $@ test_lens.cpp $(CORE) $(LDLIBS)

run: test_wrap test_core test_lens
	./test_core $(DICT)
	./test_wrap $(DICT)
	./test_lens $(DICT)

clean:
	rm -f test_core test_net bmp_tool test_wrap test_lens libvocr.a vocr_*.o

.PHONY: run clean
