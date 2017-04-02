#include "encoder.h"

char TAG[] = "NativeEncoder";
encoder_t *self;
jclass cameraClass;
jmethodID addCallbackBuffer;

jclass streamingEncoderClass;
jmethodID onEncodedFrame;


int64_t lastCheck = 0;
int framesCount = 0;
int durationSum = 0;
void measurePerformance(int64_t time_start_ns) {
    int64_t time_start_ms = (time_start_ns / 1000000);
    int64_t now_ms = (getTimeNsec() / 1000000);
    int duration = (int)(now_ms - time_start_ms);

    framesCount++;
    durationSum += duration;

    int delta = (int)(now_ms - lastCheck);
    if (delta >= 1000) {
        int avgDuration = durationSum / framesCount;
        int busyTime = (100 * durationSum) / delta;
        LOGI(TAG, "avg duration per frame %d ms, %d%% busy, encoding at %d fps",
             avgDuration,
             busyTime,
             framesCount);
        // Reset timers
        lastCheck = now_ms;
        framesCount = 0;
        durationSum = 0;
    }
}

int64_t getTimeNsec() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec*1000000000LL + now.tv_nsec;
}

extern "C" {
    JNIEXPORT jstring JNICALL Java_gq_icctv_icctv_StreamingEncoder_getConfiguration(JNIEnv *env, jobject) {
        char info[10000] = {0};
        sprintf(info, "%s\n", avcodec_configuration());
        return env->NewStringUTF(info);
    }

    JNIEXPORT void JNICALL Java_gq_icctv_icctv_StreamingEncoder_nativeInitialize(JNIEnv *env, jobject, int in_width, int in_height, int out_width, int out_height, int bitrate) {
        LOGI(TAG, "Initializing");

        self = (encoder_t *) malloc(sizeof(encoder_t));
        memset(self, 0, sizeof(encoder_t));

        self->in_width = in_width;
        self->in_height = in_height;
        self->out_width = out_width;
        self->out_height = out_height;

        avcodec_register_all();

        self->codec = avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);

        self->context = avcodec_alloc_context3(self->codec);
        self->context->dct_algo = FF_DCT_FASTINT;
        self->context->bit_rate = bitrate;
        self->context->width = out_width;
        self->context->height = out_height;
        self->context->time_base.num = 1;
        self->context->time_base.den = 30;
        self->context->gop_size = 10;
        self->context->max_b_frames = 0;
        self->context->pix_fmt = AV_PIX_FMT_YUV420P;

        avcodec_open2(self->context, self->codec, NULL);

        self->frame = av_frame_alloc();
        self->frame->format = AV_PIX_FMT_YUV420P;
        self->frame->width  = out_width;
        self->frame->height = out_height;
        self->frame->pts = 0;

        // The magic constant 32 is the line size alignment which allows SIMD
        int frame_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, out_width, out_height, 32);

        LOGI(TAG, "Allocating %d bytes for frame buffer", frame_size);

        self->frame_buffer = malloc(frame_size);

        avpicture_fill((AVPicture*) self->frame, (uint8_t*) self->frame_buffer, AV_PIX_FMT_YUV420P, out_width, out_height);

        LOGI(TAG, "After filling picture, linesize is %d, %d", self->frame->linesize[0], self->frame->linesize[1]);


        self->sws = sws_getContext(
            in_width, in_height, AV_PIX_FMT_NV21,
            out_width, out_height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, 0, 0, 0
        );

        if (!self->sws) {
            LOGE(TAG, "Could not create scale context");
        }

        // Cache the Java method to give back the preview buffer,
        // as it needs to be called after processing each frame
        // The magic string is the signature of the method, use this cmd to find it
        //     javap -classpath sdk/platforms/android-9/android.jar -s -p android.hardware.Camera
        jclass tmpCameraClass = env->FindClass("android/hardware/Camera");
        cameraClass = (jclass)env->NewGlobalRef(tmpCameraClass);
        addCallbackBuffer = env->GetMethodID(cameraClass, "addCallbackBuffer", "([B)V");

        // Also cache the callback method of the StreamingEncoder, onEncodedFrame
        jclass tmpStreamingEncoderClass = env->FindClass("gq/icctv/icctv/StreamingEncoder");
        streamingEncoderClass = (jclass)env->NewGlobalRef(tmpStreamingEncoderClass);
        onEncodedFrame = env->GetMethodID(streamingEncoderClass, "onEncodedFrame", "([B)V");

        LOGI(TAG, "Initialized");
    }

    JNIEXPORT void JNICALL Java_gq_icctv_icctv_StreamingEncoder_onPreviewFrame(JNIEnv *env, jobject streamingEncoderInstance, jbyteArray pixelsBuffer, jobject camera) {
        int64_t time_start = getTimeNsec();

        int length = env->GetArrayLength(pixelsBuffer);

        if (length < 1000) {
            LOGE(TAG, "Received only %d pixels", length);
            return;
        }

        uint8_t *pixels = (uint8_t *) env->GetByteArrayElements(pixelsBuffer, NULL);

        // The length of a stride is the width of a plane in bytes
        uint8_t stride = ((uint8_t)self->in_width) * (12 / 8);

        // Source pixels are in NV21 format: 2 planes, Y and VU
        // [Y0 Y1 Y2 Y4 ...       ] <- Y plane (luma)
        // [                      ]
        // [                      ]
        // [V0 U0 V1 U1 V2 U2 ... ] <- VU plane (chroma)
        uint8_t *in_data[2] = { pixels, pixels + stride * self->in_height };

        // In NV21, each of the two planes have the same stride (=width)
        int in_linesize[2] = { stride, stride };

        // LOGI(TAG, "Scaling in_linesize=%d~%d, in_height=%d, in_width=%d, frame_linesize=%d~%d",
        //     in_linesize[0],
        //     in_linesize[1],
        //     self->in_height,
        //     self->in_width,
        //     self->frame->linesize[0],
        //     self->frame->linesize[1]);

        // LOGI(TAG, "Scaling in_data[0]=%u, in_data[1]=%u", (unsigned int) pixels[0], (unsigned int) pixels[1]);

        // Perform pixel format conversion from NV21 to YV12 (YUV420P)
        // Scaling is fast (~1ms), don't worry about bottleneck here
        int out_height = sws_scale(self->sws,
                  in_data,
                  in_linesize,
                  0,
                  self->in_height,
                  self->frame->data,
                  self->frame->linesize);

        // LOGI(TAG, "Scaler returned output height %d", out_height);


        self->frame->pts++;
        av_init_packet(&self->packet);
        int success = 0;
        avcodec_encode_video2(self->context, &self->packet, self->frame, &success);
        if(success) {
            // memcpy(encoded_data, self->packet.data, self->packet.size);
        } else {
            LOGE(TAG, "Failed to encode frame");
        }

        // Free the array without copying back changes ("abort")
        env->ReleaseByteArrayElements(pixelsBuffer, (jbyte *) pixels, JNI_ABORT);

        // Give back the buffer to be filled again
        env->CallVoidMethod(camera, addCallbackBuffer, pixelsBuffer);

        // Pass encoded frame to Java callback function
        LOGI(TAG, "Encoded packet size %d", self->packet.size);

        if (self->packet.size > 0) {

            LOGI(TAG, "Info 0=%d, 30=%d", self->packet.data[0], self->packet.data[30]);
            jbyteArray frame = env->NewByteArray(self->packet.size);
            env->SetByteArrayRegion(frame, 0, self->packet.size - 1,
                                    (const jbyte *) (&self->packet.data));
            env->CallVoidMethod(streamingEncoderInstance, onEncodedFrame, frame);
        }
        av_free_packet(&self->packet);

        measurePerformance(time_start);
    }


    JNIEXPORT void JNICALL Java_gq_icctv_icctv_StreamingEncoder_nativeRelease(JNIEnv *env, jobject) {
        if (self == NULL) { return; }

        LOGI(TAG, "Releasing");
        sws_freeContext(self->sws);
        avcodec_close(self->context);
        av_free(self->context);
        av_free(self->frame);
        free(self->frame_buffer);
        free(self);
        LOGI(TAG, "Released");
    }
}
