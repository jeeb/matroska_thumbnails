#ifndef MT_ISTREAM_WRAPPER_H
#define MT_ISTREAM_WRAPPER_H

#include <stdint.h>

int read_packet(void *opaque, uint8_t *buf, int buf_size);

int64_t seek(void *opaque, int64_t offset, int whence);

#endif /* MT_ISTREAM_WRAPPER_H */
