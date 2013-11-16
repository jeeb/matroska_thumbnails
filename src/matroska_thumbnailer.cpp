#include <stdint.h>
#include <thumbcache.h>
#include <propsys.h>
#include <shlwapi.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <libswscale/swscale.h>
}

class MatroskaThumbnailer : public IThumbnailProvider,
                            public IInitializeWithStream
{
public:
    MatroskaThumbnailer() : reference_count(1), stream(nullptr)
    {
    }

    virtual ~MatroskaThumbnailer()
    {
        if (stream) {
            stream->Release();
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
        ULONG cur_ref = InterlockedIncrement(&reference_count);
        if (!cur_ref) {
            delete this;
        }

        return cur_ref;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream *pStream, DWORD grfMode);

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha);

private:
    uint32_t reference_count;
    IStream  *stream;
};

IFACEMETHODIMP MatroskaThumbnailer::Initialize(IStream *pStream, DWORD grfMode)
{
    HRESULT hr = E_UNEXPECTED;

    // Only initialize if the class stream is not yet initialized
    if (!stream) {
        hr = pStream->QueryInterface(&stream);
    }

    return hr;
}

#define BUFFER_SIZE 8192
static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    IStream *istream = (IStream *)opaque;

    HRESULT hr         = E_UNEXPECTED;
    ULONG   read_bytes = 0;

    hr = istream->Read(buf, buf_size, &read_bytes);

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

    switch (whence) {
    // Try using the stat thingy to grab the stream's size
    case AVSEEK_SIZE:
        hr = istream->Stat(&stream_stats, STATFLAG_NONAME);
        if (SUCCEEDED(hr)) {
            return stream_stats.cbSize.QuadPart;
        } else {
            return -1;
        }
        break;
    case SEEK_SET:
        seekmode = STREAM_SEEK_SET;
        break;
    case SEEK_CUR:
        seekmode = STREAM_SEEK_CUR;
        break;
    case SEEK_END:
        seekmode = STREAM_SEEK_END;
        break;
    default:
        break;
    }

    // Try actually seeking
    hr = istream->Seek(seek_offset, seekmode, &pos_after_seek);

    // Return the difference in positions
    return pos_before_seek.QuadPart - pos_after_seek.QuadPart;
}

IFACEMETHODIMP MatroskaThumbnailer::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha)
{
    // Register all formats etc.
    av_register_all();

    // Create the lavf context
    AVFormatContext *lavf_context = avformat_alloc_context();
    if (!lavf_context) {
        return E_OUTOFMEMORY;
    }

    // Create our buffer
    uint8_t *lavf_iobuffer = (uint8_t *)av_malloc(BUFFER_SIZE);
    if (!lavf_iobuffer) {
        return E_OUTOFMEMORY;
    }

    // Create our custom IO context
    lavf_context->pb = avio_alloc_context(lavf_iobuffer, BUFFER_SIZE, 0, stream, read_packet, NULL, seek);
    if (!lavf_context->pb) {
        return E_OUTOFMEMORY;
    }

    // Try opening the input
    int ret = avformat_open_input(&lavf_context, "fake_file_name.mkv", NULL, NULL);
    if (ret) {
        return E_FAIL;
    }

    // Try finding out what's inside the input
    ret = avformat_find_stream_info(lavf_context, NULL);
    if (ret < 0) {
        return E_FAIL;
    }

    // Try looking for the "best" video stream in file
    AVCodec *decoder = nullptr;
    ret = av_find_best_stream(lavf_context, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, NULL);
    if (ret < 0) {
        return E_UNEXPECTED;
    }

    // If no decoder was found, error out
    if (!decoder) {
        return E_NOTIMPL;
    }

    // Gather information on the found stream
    int stream_index = ret;
    AVStream *stream = lavf_context->streams[stream_index];
    AVCodecContext *decoder_context = stream->codec;

    // We want to try them refcounted frames!
    AVDictionary *avdict = nullptr;
    ret = av_dict_set(&avdict, "refcounted_frames", "1", 0);
    if (ret < 0) {
        return E_FAIL;
    }

    // Open ze decoder!
    ret = avcodec_open2(decoder_context, decoder, &avdict);
    if (ret < 0) {
        return E_FAIL;
    }

    // Create the decoded picture buffer
    uint8_t *video_dst_data[4] = { NULL };
    int  video_dst_linesize[4];
    ret = av_image_alloc(video_dst_data, video_dst_linesize,
                         decoder_context->width, decoder_context->height,
                         decoder_context->pix_fmt, 16);
    if (ret < 0) {
        return E_OUTOFMEMORY;
    }

    // Create an AVFrame
    AVFrame *frame = nullptr;
    frame = av_frame_alloc();
    if (!frame) {
        return E_OUTOFMEMORY;
    }

    // Create and init an AVPacket
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    // Go grab a "frame" from the file!
    ret = av_read_frame(lavf_context, &packet);
    if (ret < 0) {
        return E_FAIL;
    }

    // Try decoding until we can has a picture
    int can_has_picture = 0;
    do {
        ret = avcodec_decode_video2(decoder_context, frame, &can_has_picture, &packet);
        if (ret < 0) {
            return E_FAIL;
        }
    } while (!can_has_picture);

    // Allocate an AVPicture for the RGB version
    AVPicture pic;
    ret = avpicture_alloc(&pic, AV_PIX_FMT_RGB32, frame->width, frame->height);
    if (ret < 0) {
        return E_OUTOFMEMORY;
    }

    SwsContext *swscale_context = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                                 frame->width, frame->height, AV_PIX_FMT_RGB32,
                                                 SWS_BICUBIC, NULL, NULL, NULL);
    if (!swscale_context) {
        return E_FAIL;
    }

    // Convert!
    ret = sws_scale(swscale_context, frame->data, frame->linesize, 0,
                    frame->height, pic.data, pic.linesize);
    if (ret != frame->height) {
        return E_FAIL;
    }

    // I think we're done? Add resizing to aspect ratio and that's it?

    return S_OK;
}
