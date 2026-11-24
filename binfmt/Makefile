CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-error=unused-variable -Werror -g
TARGET  = binfmt

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# acceptance test 7: run under ASan — make asan && ./binfmt-asan ./broken
asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: $(TARGET)
	mv $(TARGET) $(TARGET)-asan

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET)-asan

.PHONY: clean asan
