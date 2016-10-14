TARGET = res_freeze_check.so
OBJECTS = res_freeze_check.o
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Winit-self -Wmissing-format-attribute -Wformat=2 -g -fPIC \
	-D'_GNU_SOURCE' -D'AST_MODULE="res_freeze_check"' -D'AST_MODULE_SELF_SYM=__internal_res_freeze_check_self'
LDFLAGS = -Wall -shared

.PHONY: install

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)/usr/lib/asterisk/modules
	install -m 644 $(TARGET) $(DESTDIR)/usr/lib/asterisk/modules/
