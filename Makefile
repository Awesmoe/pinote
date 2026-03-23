CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lpthread -lm
TARGET = notes_server
SRCS = notes_server.c fb_draw.c config.c api_fetch.c chart.c rss.c sprite.c forecast.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c pinote.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
