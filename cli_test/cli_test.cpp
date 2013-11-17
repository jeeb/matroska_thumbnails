#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdint.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <libswscale/swscale.h>
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s input_file output_file\n", argv[0]);
        return 1;
    }

    FILE *dst_file;
    dst_file = fopen(argv[2], "wb");
    if (!dst_file) {
        fprintf(stderr, "Failed to open output file %s\n", argv[2]);
        return 1;
    }

    fprintf(stderr, "Success: Opened output file %s\n", argv[2]);

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

        // Video decoders always consume the whole packet, so we don't have to check for that
        ret = avcodec_decode_video2(decoder_context, frame, &can_has_picture, &packet);
        if (ret < 0) {
            fprintf(stderr, "Failed to decode video :<\n");
            return 1;
        }

        fprintf(stderr, "Success: A frame of data has been decoded\n");

        av_free_packet(&packet);
    }

    fprintf(stderr, "Success: A whole picture has been decoded\n");

    fprintf(stderr, "SAR: %d:%d\n", frame->sample_aspect_ratio.num, frame->sample_aspect_ratio.den);

    int dst_width  = 0;
    int dst_height = 0;

    if (frame->sample_aspect_ratio.num > frame->sample_aspect_ratio.den) {
        dst_width  = (frame->width * frame->sample_aspect_ratio.num) / frame->sample_aspect_ratio.den;
        dst_height = frame->height;
    } else {
        dst_height = (frame->height * frame->sample_aspect_ratio.den) / frame->sample_aspect_ratio.num;
        dst_width  = frame->width;
    }

    fprintf(stderr, "DSTWidth: %d , DSTHeight: %d\n", dst_width, dst_height);
    /*
    // Allocate an AVPicture for the RGB version
    AVPicture pic;
    ret = avpicture_alloc(&pic, AV_PIX_FMT_RGB32, frame->width, frame->height);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate the output RGB picture :<\n");
        return 1;
    }
    */

    uint8_t *dst_data[4];
    int dst_linesize[4];

    ret = av_image_alloc(dst_data, dst_linesize, dst_width, dst_height, AV_PIX_FMT_RGBA, 1);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate the output RGB picture :<\n");
        return 1;
    }

    int dst_image_size = ret;

    fprintf(stderr, "Success: The output RGB picture has been allocated\n");

    SwsContext *swscale_context = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                                 dst_width, dst_height, AV_PIX_FMT_RGBA,
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

    fwrite(dst_data[0], 1, dst_image_size, dst_file);
    fclose(dst_file);

    // I think we're done? Add resizing to aspect ratio and that's it?

    return 0;
}
