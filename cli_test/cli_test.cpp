#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>
#include <inttypes.h>

// HBITMAP stuff
#include <windows.h>
#include <wingdi.h>

// IStream stuff
#include <shlwapi.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <libswscale/swscale.h>
}

// Picked off the internets for quick testing
void SaveBitmap(char *szFilename, HBITMAP hBitmap)
{
    HDC                 hdc = NULL;
    FILE*               fp = NULL;
    LPVOID              pBuf = NULL;
    BITMAPINFO          bmpInfo;
    BITMAPFILEHEADER    bmpFileHeader;

    do{

        hdc = GetDC(NULL);

        ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));

        bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

        GetDIBits(hdc, hBitmap, 0, 0, NULL, &bmpInfo, DIB_RGB_COLORS);

        if (bmpInfo.bmiHeader.biSizeImage <= 0)
            bmpInfo.bmiHeader.biSizeImage = bmpInfo.bmiHeader.biWidth*abs(bmpInfo.bmiHeader.biHeight)*(bmpInfo.bmiHeader.biBitCount + 7) / 8;

        if ((pBuf = malloc(bmpInfo.bmiHeader.biSizeImage)) == NULL)
        {
            MessageBox(NULL, "Unable to Allocate Bitmap Memory", "Error", MB_OK | MB_ICONERROR);
            break;
        }

        bmpInfo.bmiHeader.biCompression = BI_RGB;

        GetDIBits(hdc, hBitmap, 0, bmpInfo.bmiHeader.biHeight, pBuf, &bmpInfo, DIB_RGB_COLORS);

        if ((fp = fopen(szFilename, "wb")) == NULL)
        {
            MessageBox(NULL, "Unable to Create Bitmap File", "Error", MB_OK | MB_ICONERROR);
            break;
        }

        bmpFileHeader.bfReserved1 = 0;

        bmpFileHeader.bfReserved2 = 0;

        bmpFileHeader.bfSize = sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)+bmpInfo.bmiHeader.biSizeImage;

        bmpFileHeader.bfType = 'MB';

        bmpFileHeader.bfOffBits = sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);

        fwrite(&bmpFileHeader, sizeof(BITMAPFILEHEADER), 1, fp);

        fwrite(&bmpInfo.bmiHeader, sizeof(BITMAPINFOHEADER), 1, fp);

        fwrite(pBuf, bmpInfo.bmiHeader.biSizeImage, 1, fp);

    } while (false);

    if (hdc)     ReleaseDC(NULL, hdc);

    if (pBuf)    free(pBuf);

    if (fp)      fclose(fp);
}

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    IStream *istream = (IStream *)opaque;

    HRESULT hr         = E_UNEXPECTED;
    ULONG   read_bytes = 0;

    hr = istream->Read(buf, buf_size, &read_bytes);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Read: Failed to read %d bytes\n", buf_size);
        return -1;
    }

    fprintf(stderr, "IStreamIO Read: Succeeded at reading %lu bytes out of %d\n", read_bytes, buf_size);
    return read_bytes;
}

static int64_t seek(void *opaque, int64_t offset, int whence)
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
    hr = istream->Seek(zero, STREAM_SEEK_CUR, &pos_before_seek);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Seek: Failed to get current position :<\n");
        return -1;
    }

    switch (whence) {
    // Try using the stat thingy to grab the stream's size
    case AVSEEK_SIZE:
        hr = istream->Stat(&stream_stats, STATFLAG_NONAME);
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
    hr = istream->Seek(seek_offset, seekmode, &pos_after_seek);
    if (FAILED(hr)) {
        fprintf(stderr, "IStreamIO Seek: Actual seek failed :<\n");
        return -1;
    }

    fprintf(stderr, "IStreamIO Seek: Succeeded at seeking %"PRId64" bytes (requested: %"PRId64" bytes)\n", (pos_after_seek.QuadPart - pos_before_seek.QuadPart), offset);

    // Return the difference in positions
    return pos_after_seek.QuadPart - pos_before_seek.QuadPart;
}

#define CLI_TEST_BUFFER_SIZE 8192

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s input_file output_file max_width_or_height\n", argv[0]);
        return 1;
    }
    int size_limit = atoi(argv[3]);

    HRESULT hr = E_FAIL;

    // Register all formats etc.
    av_register_all();

    // Create the lavf context
    AVFormatContext *lavf_context = avformat_alloc_context();
    if (!lavf_context) {
        fprintf(stderr, "Failed to create lavf context :<\n");
        return 1;
    }

    fprintf(stderr, "Success: lavf context\n");

    // We need a wchar version of the input file name for IStream
    wchar_t *wide_input_file = L"lav_tep_jitter.vob";
    fwprintf(stderr, L"Wide input file: %ls\n", wide_input_file);

    // Create an IStream that doesn't create files
    IStream *istream = nullptr;
    hr = SHCreateStreamOnFileEx(wide_input_file, STGM_FAILIFTHERE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &istream);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create the IStream :<\n");
        return 1;
    }

    fprintf(stderr, "Success: IStream created!\n");

    // Create our buffer for custom lavf IO
    uint8_t *lavf_iobuffer = (uint8_t *)av_malloc(CLI_TEST_BUFFER_SIZE);
    if (!lavf_iobuffer) {
        return E_OUTOFMEMORY;
    }

    // Create our custom IO context
    lavf_context->pb = avio_alloc_context(lavf_iobuffer, CLI_TEST_BUFFER_SIZE, 0,
                                          istream, read_packet, NULL, seek);
    if (!lavf_context->pb) {
        return E_OUTOFMEMORY;
    }

    // Try opening the input
    int ret = avformat_open_input(&lavf_context, "fake_video_name", NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open input file :<\n");
        return 1;
    }

    fprintf(stderr, "Success: lavf file opening\n");

    // Try finding out what's inside the input
    ret = avformat_find_stream_info(lavf_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to find out what's inside the file :<\n");
        return 1;
    }

    fprintf(stderr, "Success: lavf finding out what's inside\n");

    // Try looking for the "best" video stream in file
    AVCodec *decoder = nullptr;
    ret = av_find_best_stream(lavf_context, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to find the best video stream :<\n");
        return 1;
    }

    // If no decoder was found, error out
    if (!decoder) {
        fprintf(stderr, "Failed to find a decoder for the best video stream :<\n");
        return 1;
    }

    fprintf(stderr, "Success: lavf finds the best video stream and there is a decoder\n");

    // Gather information on the found stream
    int stream_index = ret;
    AVStream *stream = lavf_context->streams[stream_index];
    AVCodecContext *decoder_context = stream->codec;

    // We want to try them refcounted frames!
    AVDictionary *avdict = nullptr;
    ret = av_dict_set(&avdict, "refcounted_frames", "1", 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create an AVDict with the refcounted_frames set to 1\n");
        return 1;
    }

    fprintf(stderr, "Success: Refcounted frames turned on\n");

    // Open ze decoder!
    ret = avcodec_open2(decoder_context, decoder, &avdict);
    if (ret < 0) {
        fprintf(stderr, "Failed to open video decoder\n");
        return 1;
    }

    fprintf(stderr, "Success: Video decoder opened\n");

    // Create an AVFrame
    AVFrame *frame = nullptr;
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Failed to allocate AVFrame :<\n");
        return 1;
    }

    // Create and init an AVPacket
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    fprintf(stderr, "Success: AVPacket created and initialized\n");

    // A marker for if we already have a decoded picture
    int can_has_picture = 0;

    while (!can_has_picture) {
        // Go grab a "frame" from the file!
        ret = av_read_frame(lavf_context, &packet);
        if (ret < 0) {
            fprintf(stderr, "Failed to read a frame of data from the input :<\n");
            return 1;
        }

        fprintf(stderr, "Success: A frame of data has been read from the input\n");

        if (packet.stream_index == stream_index) {
            // Video decoders always consume the whole packet, so we don't have to check for that
            ret = avcodec_decode_video2(decoder_context, frame, &can_has_picture, &packet);
            if (ret < 0) {
                fprintf(stderr, "Failed to decode video :<\n");
                return 1;
            }

            fprintf(stderr, "Success: A frame of data has been decoded\n");
        }

        av_free_packet(&packet);
    }

    fprintf(stderr, "Success: A whole picture has been decoded\n");
    AVRational guessed_sar = av_guess_sample_aspect_ratio(lavf_context, stream, frame);
    fprintf(stderr, "Stream SAR: %d:%d\n", frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den);
    fprintf(stderr, "Guessed SAR: %d:%d\n", guessed_sar.num, guessed_sar.den);

    int dst_width = 0;
    int dst_height = 0;

    if (guessed_sar.num > guessed_sar.den) {
        dst_width = (frame->width * guessed_sar.num) / guessed_sar.den;
        dst_height = frame->height;
    }
    else {
        dst_height = (frame->height * guessed_sar.den) / guessed_sar.num;
        dst_width = frame->width;
    }

    fprintf(stderr, "DSTWidth: %d , DSTHeight: %d\n", dst_width, dst_height);

    // Fit the thing into size_limit
    if (dst_width > dst_height) {
        double ratio = (double)dst_height / (double)dst_width;
        dst_width = size_limit;
        dst_height = (int)((ratio * (double)size_limit) + 0.5);
    }
    else {
        double ratio = (double)dst_width / (double)dst_height;
        dst_height = size_limit;
        dst_width = (int)((ratio * (double)size_limit) + 0.5);
    }

    fprintf(stderr, "DSTWidth: %d , DSTHeight: %d (post-fitting)\n", dst_width, dst_height);

    uint8_t *dst_data[4] = { nullptr };
    int      dst_linesize[4] = { 0 };

    // The linesize is padded to the next 4 byte alignment
    // But we have four values next to each other so we
    // don't care
    dst_linesize[0] = dst_width * 4;

    fprintf(stderr, "Linesize: %d\n", dst_linesize[0]);

    // Create a BITMAPINFO structure to create the bitmap with
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));

    // Set the values to the header
    bmi.bmiHeader.biSize = sizeof(BITMAPINFO);
    bmi.bmiHeader.biWidth  = dst_width;
    bmi.bmiHeader.biHeight = -dst_height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed = 0;
    bmi.bmiHeader.biClrImportant = 0;

    // Get the screen... whyyyyy
    HDC hdc = GetDC(NULL);

    // Create a Windows HBITMAP bitmap
    HBITMAP dst_bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)dst_data, NULL, 0);
    if (!dst_bitmap || !dst_data) {
        fprintf(stderr, "Failed to create the HBITMAP :<\n");
        return 1;
    }

    if ((long)dst_bitmap == ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "Invalid parameters with the DIBSection, ho!\n");
    }

    fprintf(stderr, "Success: The output RGB picture has been allocated\n");

    SwsContext *swscale_context = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                                 dst_width, dst_height, AV_PIX_FMT_BGRA,
                                                 SWS_BICUBIC, NULL, NULL, NULL);
    if (!swscale_context) {
        fprintf(stderr, "Failed to create the swscale context for the YCbCr->RGB conversion\n");
        return 1;
    }

    // Convert!
    ret = sws_scale(swscale_context, frame->data, frame->linesize, 0,
                    frame->height, dst_data, dst_linesize);
    if (ret != dst_height) {
        fprintf(stderr, "Failed to gain as much height as with the input when scaling\n");
        return 1;
    }

    fprintf(stderr, "Success: Picture has been scaled\n");

    SaveBitmap(argv[2], dst_bitmap);

    // We don't need the stinking screen
    ReleaseDC(NULL, hdc);

    return 0;
}
