#include "survive_str.h"
#include "string.h"
#include <assert.h>
#include <stdarg.h>
#include <survive.h>

void str_ensure_size(cstring *str, size_t s) {
	s++;
	if (str->size >= s)
		return;
	size_t realloc_size = str->size + 1024;
	if (s > realloc_size)
		realloc_size = s;
	str->d = SV_REALLOC(str->d, realloc_size);
	str->d[str->length] = 0;
	str->size = realloc_size;
}

char *str_increase_by(cstring *str, size_t len) {
	str_ensure_size(str, len + str->length);
	str->length = len + str->length;
	return str->d + str->length - len;
}
void str_free(cstring *str) {
	free(str->d);
	cstring newStr = {0,0,0};
	*str = newStr;
}
void str_append(cstring *str, const char *add) {
	size_t add_len = strlen(add);
	strcat(str_increase_by(str, add_len), add);
	assert(strlen(str->d) == str->length);
}

int str_append_printf(cstring *str, const char *format, ...) {
	va_list args;
	va_start(args, format);
	size_t needed = vsnprintf(0, 0, format, args);
	va_end(args);
	va_start(args, format);
	int rtn = vsnprintf(str_increase_by(str, needed + 1), needed + 1, format, args);
	va_end(args);
	str->length -= (needed + 1 - rtn);
	assert(strlen(str->d) == str->length);
	return rtn;
}
