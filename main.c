/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for decoding and filtering
 * @example filtering_video.c
 */

#define _XOPEN_SOURCE 600 /* for usleep */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

const char *filter_descr = "[in]scale=300:100[scl];[in1][scl]overlay=25:25";
/* other way:
   scale=78:24 [scl]; [scl] transpose=cclock // assumes "[in]" and "[out]" to be input output pads respectively
 */
static AVFormatContext *liveStreamOutputContext;
static AVFormatContext *inputFormatContext;
static AVFormatContext *logoFormatContext;

static AVCodecContext *decoderContext;
static AVCodecContext *videoCodecContext;
static AVCodecContext *logoCodecContext;

static AVCodec *videoCodec;
static AVCodec *audioCodec;

static AVStream *audioStream;
static AVStream *videoStream;

AVFilterContext *bufferSinkContext;
AVFilterContext *bufferSourceContext;
AVFilterContext *bufferSource1Context;
AVFilterGraph *filterGraph;
static int videoStreamIndex = -1;
static int audioStreamIndex = -1;
static int64_t lastPts = AV_NOPTS_VALUE;
static int64_t startTime;

static int open_input_file(const char *filename)
{
    int ret;
    AVCodec *dec;
    AVCodec *logoDec;

//    if((ret = avformat_open_input(&logoFormatContext, "logo.png", NULL, NULL)) < 0)
//    {
//        av_log(NULL, AV_LOG_ERROR, "Cannot open logo input file\n");
//        return ret;
//    }

//    if ((ret = avformat_find_stream_info(logoFormatContext, NULL)) < 0) {
//        av_log(NULL, AV_LOG_ERROR, "Cannot find logo stream information\n");
//        return ret;
//    }

//    ret = av_find_best_stream(logoFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &logoDec, 0);
//    if (ret < 0) {
//        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the logo input file\n");
//        return ret;
//    }

    if ((ret = avformat_open_input(&inputFormatContext, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(inputFormatContext, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(inputFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    videoStreamIndex = ret;

    ret = av_find_best_stream(inputFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a audio stream in the input file\n");
        return ret;
    }
    audioStreamIndex = ret;

    videoCodecContext = avcodec_alloc_context3(videoCodec);
    if (!videoCodecContext)
        return AVERROR(ENOMEM);
//    avcodec_parameters_to_context(videoCodecContext, inputFormatContext->streams[videoStreamIndex]->codecpar);
    videoCodecContext->time_base = inputFormatContext->streams[videoStreamIndex]->time_base;
    printf("videoCodecCtx->time_base.den : %d\n", videoCodecContext->time_base.den);
    printf("videoCodecCtx->time_base.num : %d\n", videoCodecContext->time_base.num);

    videoCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    videoCodecContext->sample_fmt = AV_SAMPLE_FMT_S16;
    videoCodecContext->width = 1920;
    videoCodecContext->height = 1080;
    videoCodecContext->bit_rate = 500000;
    videoCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    videoCodecContext->profile = FF_PROFILE_H264_MAIN;
    videoCodecContext->level = 41;
    videoCodecContext->thread_count = 8;

    /* create decoding context */
    decoderContext = avcodec_alloc_context3(dec);
    if (!decoderContext)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(decoderContext, inputFormatContext->streams[videoStreamIndex]->codecpar);

    /* init the video decoder */
    if ((ret = avcodec_open2(decoderContext, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }

    return 0;
}

static int init_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersrc1 = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *overlayFrameInOut = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = inputFormatContext->streams[videoStreamIndex]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    filterGraph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filterGraph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             decoderContext->width, decoderContext->height, decoderContext->pix_fmt,
             time_base.num, time_base.den,
             decoderContext->sample_aspect_ratio.num, decoderContext->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&bufferSourceContext, buffersrc, "in",
                                       args, NULL, filterGraph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    ret = avfilter_graph_create_filter(&bufferSource1Context, buffersrc1, "in1",
                                       args, NULL, filterGraph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&bufferSinkContext, buffersink, "out",
                                       NULL, NULL, filterGraph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(bufferSinkContext, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = bufferSourceContext;
    outputs->pad_idx    = 0;
    outputs->next       = overlayFrameInOut;

    overlayFrameInOut->name = av_strdup("in1");
    overlayFrameInOut->filter_ctx = bufferSource1Context;
    overlayFrameInOut->pad_idx = 0;
    overlayFrameInOut->next = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = bufferSinkContext;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filterGraph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filterGraph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static void display_frame(const AVFrame *frame, AVRational time_base)
{
    int x, y;
    uint8_t *p0, *p;
    int64_t delay;
    int code;
    int gotOutput;

    AVPacket packet = {0};
    av_init_packet(&packet);

//    if (frame->pts != AV_NOPTS_VALUE) {
//        if (lastPts != AV_NOPTS_VALUE) {
//            /* sleep roughly the right amount of time;
//             * usleep is in microseconds, just like AV_TIME_BASE. */
//            AVRational avTimeBaseQ = {1, AV_TIME_BASE};
//            delay = av_rescale_q(frame->pts - lastPts,
//                                 videoCodecContext->time_base, avTimeBaseQ);
//            if (delay > 0 && delay < 1000000)
//                usleep(delay);
//        }
//        lastPts = frame->pts;
//    }

    /* Trivial ASCII grayscale display. */
    //    p0 = frame->data[0];
    //    puts("\033c");
    //    for (y = 0; y < frame->height; y++) {
    //        p = p0;
    //        for (x = 0; x < frame->width; x++)
    //            putchar(" .-+#"[*(p++) / 52]);
    //        putchar('\n');
    //        p0 += frame->linesize[0];
    //    }
    //    fflush(stdout);

    code = avcodec_encode_video2(videoCodecContext, &packet, frame, &gotOutput);
    if(code < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] Encode frame -> avcodec_encode_video2()");
    }

    AVRational avTimeBaseQ = {1, AV_TIME_BASE};
    int64_t ptsTime = av_rescale_q(frame->pts, inputFormatContext->streams[videoStreamIndex]->time_base, avTimeBaseQ);
    int64_t nowTime = av_gettime() - startTime;

    if((ptsTime > nowTime)) // Important Delay to Read Files at their Native Frame Rate
    {
        int64_t sleepTime = ptsTime - nowTime;
//            sleepTime -= 30000;
        printf("Sleeping for : %d\n", sleepTime);
        av_usleep((sleepTime));
    }
    else
    {
        printf("not sleeping\n");
    }

    if(gotOutput)
    {
        packet.pts = av_rescale_q_rnd(packet.pts, time_base, videoStream->time_base, (AV_ROUND_INF|AV_ROUND_PASS_MINMAX));
        packet.dts = av_rescale_q_rnd(packet.dts, time_base, videoStream->time_base, (AV_ROUND_INF|AV_ROUND_PASS_MINMAX));
        packet.stream_index = videoStream->index;

//        if (packet.dts != AV_NOPTS_VALUE) {
//            if (lastPts != AV_NOPTS_VALUE) {
//                /* sleep roughly the right amount of time;
//                 * usleep is in microsecon
//                 * ds, just like AV_TIME_BASE. */
//                AVRational avTimeBaseQ = {1, AV_TIME_BASE};
//                delay = av_rescale_q(packet.dts - lastPts,
//                                     time_base, avTimeBaseQ);
//                if (delay > 0 && delay < 1000000)
//                    av_usleep(delay);
//                    printf("Delaying : %d\n", delay);
//            }
//            lastPts = packet.dts;
//        }



//        AVRational avTimeBaseQ = {1, AV_TIME_BASE};
//        int64_t ptsTime = av_rescale_q(packet.dts, inputFormatContext->streams[videoStreamIndex]->time_base, avTimeBaseQ);
//        int64_t nowTime = av_gettime() - startTime;

//        if((ptsTime > nowTime)) // Important Delay to Read Files at their Native Frame Rate
//        {
//            int64_t sleepTime = ptsTime - nowTime;
//            av_usleep((sleepTime));
//        }


//        if(videoStream->start_time == AV_NOPTS_VALUE)
//        {
//            videoStream->start_time = packet.pts;
//        }

        code = av_interleaved_write_frame(liveStreamOutputContext, &packet);
        av_packet_unref(&packet);

        if(code < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "[ERROR] Writing Live Stream Interleaved Frame");
        }
    }

}

static int initializeLiveStreamOutput()
{
    int error;
    const char *out_filename = "rtmp://127.0.0.1:1935/live";
//    const char *out_filename = "rtmp://cdn1.streamencoding.com:1935/demo_live?streamencoding=BrKOb/deneme";

    avformat_alloc_output_context2(&liveStreamOutputContext, NULL, "flv", out_filename);

    if(!liveStreamOutputContext)
    {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context!");
        return -1;
    }

    videoStream = avformat_new_stream(liveStreamOutputContext, videoCodec);
    if(!videoStream)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] Could not create Live Video Stream");
        return -1;
    }
//    videoStream->codecpar = inputFormatContext->streams[videoStreamIndex]->codecpar;
    avcodec_parameters_from_context(videoStream->codecpar, videoCodecContext);
    videoStream->codecpar->codec_tag = 0;


    audioStream = avformat_new_stream(liveStreamOutputContext, NULL);
    if(!audioStream)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] Could not create Live Audio Stream");
        return -1;
    }
    audioStream->codecpar = inputFormatContext->streams[audioStreamIndex]->codecpar;
    audioStream->codecpar->codec_tag = 0;


    av_dump_format(liveStreamOutputContext, 0, out_filename, 1);

    error = avio_open(&liveStreamOutputContext->pb, out_filename, AVIO_FLAG_WRITE);
    if(error < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] Could not open output url");
        return -2;
    }

    error = avformat_write_header(liveStreamOutputContext, NULL);
    if(error < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] Could not write header to output format context");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret;
    AVPacket packet;
    AVFrame *frame;
    AVFrame *filt_frame;

    avcodec_register_all();
    avformat_network_init();

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        exit(1);
    }

    videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if(!videoCodec)
    {
        perror("Can't find h264 encoder");
        exit(1);
    }

    audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if(!audioCodec)
    {
        perror("Can't find aac encoder");
        exit(1);
    }

    av_log(NULL, AV_LOG_INFO, "Test\n");

    int error;

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!frame || !filt_frame) {
        perror("Could not allocate frame");
        exit(1);
    }



    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;

    error = avcodec_open2(videoCodecContext, videoCodec, NULL);
    if(error < 0)
    {
        perror("[ERROR] avcodec_open2");
        exit(1);
    }

    error = av_opt_set(videoCodecContext->priv_data, "preset", "ultrafast", 0);
    if(error < 0)
    {
        perror("[ERROR] av_opt_set preset");
        exit(1);
    }


    ret = initializeLiveStreamOutput();
    if(ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "[ERROR] initializeLiveStreamOutput()\n");
        exit(ret);
    }

    startTime = av_gettime();

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(inputFormatContext, &packet)) < 0)
            break;

//        AVRational avTimeBaseQ = {1, AV_TIME_BASE};
//        int64_t ptsTime = av_rescale_q(packet.dts, inputFormatContext->streams[videoStreamIndex]->time_base, avTimeBaseQ);
//        int64_t nowTime = av_gettime() - startTime;

//        if((ptsTime > nowTime)) // Important Delay to Read Files at their Native Frame Rate
//        {
//            int64_t sleepTime = ptsTime - nowTime;
////            sleepTime -= 30000;
//            printf("Sleeping for : %d\n", sleepTime);
//            av_usleep((sleepTime));
//        }
//        else
//        {
//            printf("not sleeping\n");
//        }


        if (packet.stream_index == videoStreamIndex) { // Video Packets
            ret = avcodec_send_packet(decoderContext, &packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(decoderContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                frame->pts = frame->best_effort_timestamp;

                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame_flags(bufferSourceContext, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                    break;
                }
                if (av_buffersrc_add_frame_flags(bufferSource1Context, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph1\n");
                    break;
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
                    ret = av_buffersink_get_frame(bufferSinkContext, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                    display_frame(filt_frame, bufferSinkContext->inputs[0]->time_base);
//                    printf("bfSinkCtx->timebase.den : %d\n", bufferSinkContext->inputs[0]->time_base.den);
//                    printf("bfSinkCtx->timebase.num : %d\n", bufferSinkContext->inputs[0]->time_base.num);
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
        }
        else // Audio Packets
        {
            packet.pts = av_rescale_q_rnd(packet.pts, inputFormatContext->streams[audioStreamIndex]->time_base, audioStream->time_base, (AV_ROUND_INF|AV_ROUND_PASS_MINMAX));
            packet.dts = av_rescale_q_rnd(packet.dts, inputFormatContext->streams[audioStreamIndex]->time_base, audioStream->time_base, (AV_ROUND_INF|AV_ROUND_PASS_MINMAX));
            packet.stream_index = audioStream->index;
            av_interleaved_write_frame(liveStreamOutputContext, &packet);
        }
        av_packet_unref(&packet);
    }
end:
    avfilter_graph_free(&filterGraph);
    avcodec_free_context(&decoderContext);
    avformat_close_input(&inputFormatContext);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        exit(1);
    }

    exit(0);
}
