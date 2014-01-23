#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "istream_wrapper.h"

// IStream stuff
#include <shlwapi.h>

// For that one AV define
#define inline __inline
#include <libavformat/avio.h>

int istream_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    IStream *istream = (IStream *)opaque;

    HRESULT hr         = E_UNEXPECTED;
    ULONG   read_bytes = 0;

    hr = istream->lpVtbl->Read(istream, buf, buf_size, &read_bytes);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Read: Failed to read %d bytes\n", buf_size);
        return -1;
    }

    fprintf(stderr, "IStreamIO Read: Succeeded at reading %lu bytes out of %d\n", read_bytes, buf_size);
    return read_bytes;
}

int64_t istream_seek(void *opaque, int64_t offset, int whence)
{
    IStream *istream = (IStream *)opaque;

    STATSTG stream_stats;
    HRESULT hr       = E_UNEXPECTED;
    DWORD   seekmode = STREAM_SEEK_CUR;

    // Seek positions
    ULARGE_INTEGER pos_before_seek;
    ULARGE_INTEGER pos_after_seek;

    LARGE_INTEGER zero;
    zero.QuadPart = 0;

    // Because MS decided to use LARGE_INTEGER...
    LARGE_INTEGER seek_offset;
    seek_offset.QuadPart = offset;

    // Grab current position
    hr = istream->lpVtbl->Seek(istream, zero, STREAM_SEEK_CUR, &pos_before_seek);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Seek: Failed to get current position :<\n");
        return -1;
    }

    switch (whence) {
    // Try using the stat thingy to grab the stream's size
    case AVSEEK_SIZE:
        hr = istream->lpVtbl->Stat(istream, &stream_stats, STATFLAG_NONAME);
        if (SUCCEEDED(hr)) {
            fprintf(stderr, "IStreamIO Seek: Succeeded at getting the stream size (reported value %llu)\n", stream_stats.cbSize.QuadPart);
            return stream_stats.cbSize.QuadPart;
        } else {
            fprintf(stderr, "IStreamIO Seek: Failed to get the stream size :<\n");
            return -1;
        }
        break;
    case SEEK_SET:
        fprintf(stderr, "IStreamIO Seek: Seek mode set to SET\n");
        seekmode = STREAM_SEEK_SET;
        break;
    case SEEK_CUR:
        fprintf(stderr, "IStreamIO Seek: Seek mode set to CUR\n");
        seekmode = STREAM_SEEK_CUR;
        break;
    case SEEK_END:
        fprintf(stderr, "IStreamIO Seek: Seek mode set to END\n");
        seekmode = STREAM_SEEK_END;
        break;
    default:
        break;
    }

    // Try actually seeking
    hr = istream->lpVtbl->Seek(istream, seek_offset, seekmode, &pos_after_seek);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Seek: Actual seek failed :<\n");
        return -1;
    }

    fprintf(stderr, "IStreamIO Seek: Succeeded at seeking %"PRId64" bytes (requested: %"PRId64" bytes)\n", (pos_after_seek.QuadPart - pos_before_seek.QuadPart), offset);

    // Return the difference in positions
    return pos_after_seek.QuadPart - pos_before_seek.QuadPart;
}
