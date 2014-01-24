#include <new>

#include <stdint.h>
#include <thumbcache.h>
#include <propsys.h>
#include <shlwapi.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <libswscale/swscale.h>
#include "istream_wrapper.h"
}

class MatroskaThumbnailer : public IThumbnailProvider,
                            public IInitializeWithStream
{
public:
    MatroskaThumbnailer() : reference_count(1), istream(nullptr)
    {
    }

    virtual ~MatroskaThumbnailer()
    {
        if (istream) {
            istream->Release();
        }
    }

    // IUnknown implementation
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(MatroskaThumbnailer, IInitializeWithStream),
            QITABENT(MatroskaThumbnailer, IThumbnailProvider),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&reference_count);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        ULONG cur_refcount = InterlockedDecrement(&reference_count);
        if (!cur_refcount) {
            delete this;
        }

        return cur_refcount;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream *pStream, DWORD grfMode);

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha);

private:
    uint32_t  reference_count;
    IStream  *istream;
};

HRESULT MatroskaThumbnailer_CreateInstance(REFIID riid, void **ppv)
{
    MatroskaThumbnailer *pNew = new (std::nothrow) MatroskaThumbnailer();
    HRESULT hr = pNew ? S_OK : E_OUTOFMEMORY;

    if (SUCCEEDED(hr)) {
        hr = pNew->QueryInterface(riid, ppv);
        pNew->Release();
    }

    return hr;
}


IFACEMETHODIMP MatroskaThumbnailer::Initialize(IStream *pStream, DWORD)
{
    HRESULT hr = E_UNEXPECTED;

    // Only initialize if the class stream is not yet initialized
    if (!istream) {
        hr = pStream->QueryInterface(&istream);
    }

    return hr;
}

#define TEST_BUFFER_SIZE 8192

IFACEMETHODIMP MatroskaThumbnailer::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)
{
    HRESULT hr = E_FAIL;
    *phbmp     = nullptr;
    *pdwAlpha  = WTSAT_UNKNOWN;

    // Initialize a local variable
    AVRational guessed_sar;
    guessed_sar.den = 0;
    guessed_sar.num = 0;

    // Initialize the local context pointers
    AVCodecContext *decoder_context = nullptr;
    SwsContext     *swscale_context = nullptr;

    // Register all formats etc.
    av_register_all();

    // Create the lavf context
    AVFormatContext *lavf_context = avformat_alloc_context();
    if (!lavf_context) {
        fprintf(stderr, "Failed to create lavf context :<\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Create our buffer for custom lavf IO
    uint8_t *lavf_iobuffer = (uint8_t *)av_malloc(TEST_BUFFER_SIZE);
    if (!lavf_iobuffer) {
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Create our custom IO context
    lavf_context->pb = avio_alloc_context(lavf_iobuffer, TEST_BUFFER_SIZE, 0,
                                          istream, istream_read_packet, NULL, istream_seek);
    if (!lavf_context->pb) {
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Try opening the input
    int ret = avformat_open_input(&lavf_context, "fake_video_name", NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open input file :<\n");
        hr = E_UNEXPECTED;
        goto cleanup;
    }

    // Try finding out what's inside the input
    ret = avformat_find_stream_info(lavf_context, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to find out what's inside the file :<\n");
        hr = E_UNEXPECTED;
        goto cleanup;
    }

    // Try looking for the "best" video stream in file
    AVCodec *decoder = nullptr;
    ret = av_find_best_stream(lavf_context, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to find the best video stream :<\n");
        hr = E_UNEXPECTED;
        goto cleanup;
    }

    // If no decoder was found, error out
    if (!decoder) {
        fprintf(stderr, "Failed to find a decoder for the best video stream :<\n");
        hr = E_UNEXPECTED;
        goto cleanup;
    }

    // Gather information on the found stream
    int stream_index = ret;
    AVStream *stream = lavf_context->streams[stream_index];
    decoder_context = stream->codec;

    // We want to try them refcounted frames!
    AVDictionary *avdict = nullptr;
    ret = av_dict_set(&avdict, "refcounted_frames", "1", 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create an AVDict with the refcounted_frames set to 1\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Open ze decoder!
    ret = avcodec_open2(decoder_context, decoder, &avdict);
    if (ret < 0) {
        fprintf(stderr, "Failed to open video decoder\n");
        hr = E_FAIL;
        goto cleanup;
    }

    // Create an AVFrame
    AVFrame *frame = nullptr;
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Failed to allocate AVFrame :<\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Create and init an AVPacket
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    // A marker for if we already have a decoded picture
    int can_has_picture = 0;

    while (!can_has_picture) {
        // Go grab a "frame" from the file!
        ret = av_read_frame(lavf_context, &packet);
        if (ret < 0) {
            fprintf(stderr, "Failed to read a frame of data from the input :<\n");
            hr = E_UNEXPECTED;
            goto cleanup;
        }

        fprintf(stderr, "Success: A frame of data has been read from the input\n");

        if (packet.stream_index == stream_index) {
            // Video decoders always consume the whole packet, so we don't have to check for that
            ret = avcodec_decode_video2(decoder_context, frame, &can_has_picture, &packet);
            if (ret < 0) {
                fprintf(stderr, "Failed to decode video :<\n");
                hr = E_UNEXPECTED;
                goto cleanup;
            }

            fprintf(stderr, "Success: A frame of data has been decoded\n");
        }

        av_free_packet(&packet);
    }

    fprintf(stderr, "Success: A whole picture has been decoded\n");
    guessed_sar = av_guess_sample_aspect_ratio(lavf_context, stream, frame);
    fprintf(stderr, "Stream SAR: %d:%d\n", frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den);
    fprintf(stderr, "Guessed SAR: %d:%d\n", guessed_sar.num, guessed_sar.den);

    int dst_width = 0;
    int dst_height = 0;

    // If the guessed SAR somehow ends up zero somewhere, we reset to 1:1
    if (guessed_sar.den == 0 || guessed_sar.num == 0) {
        guessed_sar.den = 1;
        guessed_sar.num = 1;
    }

    // Calculate the aspect ratio'ized size for the output picture
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
        dst_width = cx;
        dst_height = (int)((ratio * (double)cx) + 0.5);
    }
    else {
        double ratio = (double)dst_width / (double)dst_height;
        dst_height = cx;
        dst_width = (int)((ratio * (double)cx) + 0.5);
    }

    fprintf(stderr, "DSTWidth: %d , DSTHeight: %d (post-fitting)\n", dst_width, dst_height);

    // Create the HBITMAP
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
    *phbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)dst_data, NULL, 0);
    if (!*phbmp || !dst_data) {
        fprintf(stderr, "Failed to create the HBITMAP :<\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    if ((long)*phbmp == ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "Invalid parameters with the DIBSection, ho!\n");
    }


    // Create the swscale context
    swscale_context = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                                 dst_width, dst_height, AV_PIX_FMT_BGRA,
                                                 SWS_BICUBIC, NULL, NULL, NULL);
    if (!swscale_context) {
        fprintf(stderr, "Failed to create the swscale context for the YCbCr->RGB conversion\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    // Convert!
    ret = sws_scale(swscale_context, frame->data, frame->linesize, 0,
                    frame->height, dst_data, dst_linesize);
    if (ret != dst_height) {
        fprintf(stderr, "Failed to gain as much height as with the input when scaling\n");
        hr = E_UNEXPECTED;
        goto cleanup;
    }

    // We don't need the stinking screen
    ReleaseDC(NULL, hdc);

    // Everything seems OK, folks!
    hr = S_OK;

cleanup:
    // Clean it all up, boys!
    sws_freeContext(swscale_context);
    av_frame_free(&frame);
    av_free_packet(&packet);
    avcodec_close(decoder_context);
    avformat_close_input(&lavf_context);
    avformat_free_context(lavf_context);

    return hr;
}
