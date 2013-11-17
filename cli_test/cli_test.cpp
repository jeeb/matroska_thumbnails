#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>

// HBITMAP stuff
#include <windows.h>
#include <wingdi.h>

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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s input_file output_file\n", argv[0]);
        return 1;
    }

    // Register all formats etc.
    av_register_all();

    // Create the lavf context
    AVFormatContext *lavf_context = avformat_alloc_context();
    if (!lavf_context) {
        fprintf(stderr, "Failed to create lavf context :<\n");
        return 1;
    }

    fprintf(stderr, "Success: lavf context\n");

    // Try opening the input
    int ret = avformat_open_input(&lavf_context, argv[1], NULL, NULL);
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

#define LIMITED_SIZE 128

    // Fit the thing into 32x32 (smallest thumbnail size)
    if (dst_width > dst_height) {
        double ratio = (double)dst_height / (double)dst_width;
        dst_width = LIMITED_SIZE;
        dst_height = (int)((ratio * (double)LIMITED_SIZE) + 0.5);
    }
    else {
        double ratio = (double)dst_width / (double)dst_height;
        dst_height = LIMITED_SIZE;
        dst_width = (int)((ratio * (double)LIMITED_SIZE) + 0.5);
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
                                                 dst_width, dst_height, AV_PIX_FMT_RGB32,
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
