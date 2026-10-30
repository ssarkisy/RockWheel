/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Dave Chapman
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "codeclib.h"
#include "libm4a/m4a.h"
#include "libfaad/common.h"
#include "libfaad/structs.h"
#include "libfaad/decoder.h"

CODEC_HEADER

/* The maximum buffer size handled by faad. 12 bytes are required by libfaad
 * as headroom (see libfaad/bits.c). FAAD_BYTE_BUFFER_SIZE bytes are buffered
 * for each frame. */
#define FAAD_BYTE_BUFFER_SIZE (2048-12)

/* Decoding takes much of a core at 80 MHz, at the UI's expense: on two cores
 * the COP goes through the frames on its own, into a queue the codec empties on
 * the CPU. As threads only switch when one yields, the codec gets its turn once
 * for each frame the UI draws, 40 to 100 ms, and a frame is 23 ms of audio; so
 * the codec can't wait for frames one by one, or in batches, however high its
 * priority: that fell short of real time, and the sound stopped. */
#if NUM_CORES > 1
#define DECODE_THREAD
#define DECODE_QUEUE    8
#define WINDOW_FRAMES   48
#define WINDOW_BYTES    (24*1024)
#else
#define DECODE_QUEUE    1
#define WINDOW_FRAMES   1
#define WINDOW_BYTES    FAAD_BYTE_BUFFER_SIZE
#endif

struct decoded_frame {
    NeAACDecFrameInfo info;
    void *result;
    real_t *out[2];
    bool chunk_start;
};

/* in IRAM, which both cores see the same */
static struct {
    /* the window of input: frames that are known to fit it, each with where its
       chunk starts in the file if it starts one */
    NeAACDecHandle decoder;
    unsigned char *buffer;
    size_t size;
    bool to_end_of_file;
    long pos;
    int count;
    long offsets[WINDOW_FRAMES];
    /* how far into it */
    int made;
    size_t used;
    bool over;
    /* the frames made and taken, of which those between are in the queue */
    volatile unsigned int head, tail;
    volatile bool running, stop, die;
    struct decoded_frame decoded[DECODE_QUEUE];
#ifdef DECODE_THREAD
    struct semaphore pending, ready, room;
#endif
} job IBSS_ATTR;

#ifdef DECODE_THREAD
/* the decoder's output is only there until its next frame */
static real_t decoded_out[DECODE_QUEUE][2][2*FRAME_LEN] MEM_ALIGN_ATTR;
#endif

/* The next frame of the window into the queue: false if it has no more, which
   an error or the end of what was asked for also makes the last. */
static bool decode_step(void)
{
    const int made = job.made;
    if (made == job.count)
    {
        job.over = true;
        return false;
    }
    const long offset = job.offsets[made];
    const size_t left = job.size - job.used;
    if (made > 0)
    {
        /* a chunk somewhere else, or a frame that may be cut off: another
           window, from there */
        if ((offset > 0 && offset != job.pos + (long)job.used)
            || left == 0
            || (left < FAAD_BYTE_BUFFER_SIZE && !job.to_end_of_file))
        {
            job.over = true;
            return false;
        }
    }

    const unsigned int slot = job.head % DECODE_QUEUE;
    struct decoded_frame *f = &job.decoded[slot];
    f->result = NeAACDecDecode(job.decoder, &f->info, job.buffer + job.used,
                               MIN(left, FAAD_BYTE_BUFFER_SIZE));
    f->chunk_start = offset > 0;
#ifdef DECODE_THREAD
    size_t bytes = MIN(f->info.samples >> 1, 2*FRAME_LEN) * sizeof(real_t);
    for (int ch = 0; ch < 2; ch++)
    {
        f->out[ch] = decoded_out[slot][ch];
        if (job.decoder->time_out[ch])
            memcpy(f->out[ch], job.decoder->time_out[ch], bytes);
    }
#else
    f->out[0] = job.decoder->time_out[0];
    f->out[1] = job.decoder->time_out[1];
#endif
    job.made = made + 1;
    job.used += f->info.bytesconsumed;
    if (f->result == NULL || f->info.error > 0 || f->info.bytesconsumed == 0)
        job.over = true;
    return true;
}

#ifdef DECODE_THREAD
/* Nothing on the CPU touches the decoder's memory while a window goes on, so
   the caches only need putting right around that, and for each frame's output */
static long decode_stack[(DEFAULT_STACK_SIZE + 0x2000) / sizeof(long)];
static unsigned int decode_thread_id;

static void decode_thread(void)
{
    while (1)
    {
        ci->semaphore_wait(&job.pending, TIMEOUT_BLOCK);
        if (job.die)
            break;
        /* the CPU's changes, and the new data DMA brought */
        ci->commit_discard_dcache();
        while (!job.over && !job.stop)
        {
            if (job.head - job.tail == DECODE_QUEUE)
            {
                ci->semaphore_wait(&job.room, TIMEOUT_BLOCK);
                continue;
            }
            if (decode_step())
            {
                ci->commit_dcache();
                job.head++;
                ci->semaphore_release(&job.ready);
            }
        }
        ci->commit_dcache();
        job.running = false;
        ci->semaphore_release(&job.ready);
    }
}

static void decode_thread_create(void)
{
    ci->semaphore_init(&job.pending, 1, 0);
    ci->semaphore_init(&job.ready, 1, 0);
    ci->semaphore_init(&job.room, 1, 0);
    decode_thread_id = ci->create_thread(decode_thread, decode_stack,
                                         sizeof(decode_stack), 0, "aacdec"
                                         IF_PRIO(, PRIORITY_PLAYBACK)
                                         IF_COP(, COP));
}

static void decode_thread_quit(void)
{
    if (decode_thread_id == 0)
        return;
    job.die = true;
    ci->semaphore_release(&job.pending);
    ci->thread_wait(decode_thread_id);
    ci->commit_discard_dcache();
}

/* the window set up goes on, by itself */
static void decode_run(void)
{
    ci->commit_dcache();
    ci->semaphore_release(&job.pending);
}

/* there is a frame in the queue, or the window is over */
static void decode_wait(void)
{
    while (job.head == job.tail && job.running)
        ci->semaphore_wait(&job.ready, TIMEOUT_BLOCK);
    /* the output, and nothing old of what the COP changed */
    ci->commit_discard_dcache();
}

/* the COP has room for another now */
static void decode_taken(void)
{
    job.tail++;
    ci->semaphore_release(&job.room);
}

/* no window goes on, and what is in the queue is forgotten */
static void decode_stop(void)
{
    job.stop = true;
    ci->semaphore_release(&job.room);
    while (job.running)
        ci->semaphore_wait(&job.ready, TIMEOUT_BLOCK);
    ci->commit_discard_dcache();
    job.head = job.tail = 0;
    job.stop = false;
}

/* Each track from a clean start: the COP only waits for a window now, and its
   cache is put right as that starts */
static void decode_reset(void)
{
    decode_stop();
    ci->semaphore_init(&job.ready, 1, 0);
    ci->semaphore_init(&job.room, 1, 0);
}
#else
#define decode_thread_create()
#define decode_thread_quit()
#define decode_run()
static void decode_wait(void)
{
    if (decode_step())
        job.head++;
    if (job.over)
        job.running = false;
}
#define decode_taken() (job.tail++)
static void decode_stop(void)
{
    job.running = false;
    job.head = job.tail = 0;
}
#define decode_reset() decode_stop()
#endif /* DECODE_THREAD */

/* this is the codec entry point */
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Generic codec initialisation */
        ci->configure(DSP_SET_STEREO_MODE, STEREO_NONINTERLEAVED);
        ci->configure(DSP_SET_SAMPLE_DEPTH, 29);
        decode_thread_create();
    }
    else if (reason == CODEC_UNLOAD) {
        decode_thread_quit();
    }

    return CODEC_OK;
}

/* this is called for each file to process */
enum codec_status codec_run(void)
{
    /* Note that when dealing with QuickTime/MPEG4 files, terminology is
     * a bit confusing. Files with sound are split up in chunks, where
     * each chunk contains one or more samples. Each sample in turn
     * contains a number of "sound samples" (the kind you refer to with
     * the sampling frequency).
     */
    size_t n;
    demux_res_t demux_res;
    stream_t input_stream;
    uint64_t sound_samples_done;
    uint32_t elapsed_time;
    int file_offset;
    int framelength;
    int lead_trim = 0;
    unsigned int frame_samples;
    uint32_t i;
    NeAACDecHandle decoder;
    int err;
    uint32_t seek_idx = 0;
    uint32_t s = 0;
    uint32_t sbr_fac = 1;
    unsigned char c = 0;
    long action;
    intptr_t param;
    bool empty_first_frame = false;

    /* Clean and initialize decoder structures */
    memset(&demux_res , 0, sizeof(demux_res));
    if (codec_init()) {
        LOGF("FAAD: Codec init error\n");
        return CODEC_ERROR;
    }

    action = CODEC_ACTION_NULL;
    param = ci->id3->elapsed;
    file_offset = ci->id3->offset;

    ci->configure(DSP_SET_FREQUENCY, ci->id3->frequency);
    codec_set_replaygain(ci->id3);

    stream_create(&input_stream,ci);

    ci->seek_buffer(0);

    /* if qtmovie_read returns successfully, the stream is up to
     * the movie data, which can be used directly by the decoder */
    if (!qtmovie_read(&input_stream, &demux_res)) {
        LOGF("FAAD: File init error\n");
        return CODEC_ERROR;
    }

    /* initialise the sound converter */
    decoder = NeAACDecOpen();

    if (!decoder) {
        LOGF("FAAD: Decode open error\n");
        return CODEC_ERROR;
    }

    NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration(decoder);
    conf->outputFormat = FAAD_FMT_24BIT; /* irrelevant, we don't convert */
    NeAACDecSetConfiguration(decoder, conf);

    err = NeAACDecInit2(decoder, demux_res.codecdata, demux_res.codecdata_len, &s, &c);
    if (err) {
        LOGF("FAAD: DecInit: %d, %d\n", err, decoder->object_type);
        return CODEC_ERROR;
    }

#ifdef SBR_DEC
    /* Check for need of special handling for seek/resume and elapsed time. */
    if (ci->id3->needs_upsampling_correction) {
        sbr_fac = 2;
    } else {
        sbr_fac = 1;
    }
#endif

    i = 0;

    if (param) {
        elapsed_time = param;
        action = CODEC_ACTION_SEEK_TIME;
    } else if (file_offset > 0) {
        /* Resume the desired (byte) position. Important: When resuming SBR
         * upsampling files the resulting sound_samples_done must be expanded
         * by a factor of 2. This is done via using sbr_fac. */
        if (m4a_seek_raw(&demux_res, &input_stream, file_offset,
                         &sound_samples_done, &i, &seek_idx)) {
            sound_samples_done *= sbr_fac;
        } else {
            sound_samples_done = 0;
        }
        NeAACDecPostSeekReset(decoder, i);
        elapsed_time = sound_samples_done * 1000LL / ci->id3->frequency;
    } else {
        elapsed_time = 0;
        sound_samples_done = 0;
    }

    ci->set_elapsed(elapsed_time);

    if (i == 0)
    {
        lead_trim = ci->id3->lead_trim;
    }

    decode_reset();

    /* The main decoding loop */
    while (i < demux_res.num_sample_byte_sizes) {
        if (action == CODEC_ACTION_NULL)
            action = ci->get_command(&param);

        if (action == CODEC_ACTION_HALT)
            break;

        /* Deal with any pending seek requests */
        if (action == CODEC_ACTION_SEEK_TIME) {
            decode_stop();
            /* Seek to the desired time position. Important: When seeking in SBR
             * upsampling files the seek_time must be divided by 2 when calling
             * m4a_seek and the resulting sound_samples_done must be expanded
             * by a factor 2. This is done via using sbr_fac. */
            if (m4a_seek(&demux_res, &input_stream,
                         (uint64_t) param * ci->id3->frequency / sbr_fac / 1000ULL,
                         &sound_samples_done, &i, &seek_idx)) {
                sound_samples_done *= sbr_fac;
                elapsed_time = sound_samples_done * 1000LL / ci->id3->frequency;
                ci->set_elapsed(elapsed_time);

                if (i == 0)
                {
                    lead_trim = ci->id3->lead_trim;
                }
            }
            NeAACDecPostSeekReset(decoder, i);
            ci->seek_complete();
            if (i >= demux_res.num_sample_byte_sizes)
                break;
        }

        action = CODEC_ACTION_NULL;

        if (!job.running && job.head == job.tail)
        {
            /* There can be gaps between chunks, so skip ahead if needed. It
             * doesn't seem to happen much, but it probably means that a
             * "proper" file can have chunks out of order. Why one would want
             * that an good question (but files with gaps do exist, so who
             * knows?), and we might not properly support it.
             * Metadata can also be placed after audio data so skip back if needed.
             */
            file_offset = m4a_check_sample_offset(&demux_res, i, &seek_idx);

            if (file_offset > 0 && file_offset != ci->curpos)
            {
                ci->seek_buffer(file_offset);
            }

            /* The frames after it that are known of: where a chunk starts may
             * only be looked up in order, so not with the place kept for the
             * next window, which may start before those. */
            uint32_t ahead_idx = seek_idx;
            job.offsets[0] = file_offset;
            job.count = 1;
            while (job.count < WINDOW_FRAMES
                   && i + job.count < demux_res.num_sample_byte_sizes
                   && i + job.count < demux_res.lookup_end)
            {
                job.offsets[job.count] = m4a_check_sample_offset(&demux_res,
                                                                 i + job.count,
                                                                 &ahead_idx);
                job.count++;
            }

            /* Request the required number of bytes from the input buffer */
            job.pos = ci->curpos;
            job.buffer = ci->request_buffer(&n, WINDOW_BYTES);
            job.size = n;
            job.to_end_of_file = n < WINDOW_BYTES;
            job.decoder = decoder;
            job.made = 0;
            job.used = 0;
            job.over = false;
            job.running = true;
            decode_run();
        }

        /* Decode them - returned samples will be host-endian */
        decode_wait();

        while (job.head != job.tail)
        {
            const struct decoded_frame *f =
                &job.decoded[job.tail % DECODE_QUEUE];


            /* NeAACDecDecode may sometimes return NULL without setting error. */
            if (f->result == NULL || f->info.error > 0) {
                LOGF("FAAD: decode error '%s'\n", NeAACDecGetErrorMessage(f->info.error));

                // In files with gaps between chunks and reduced lookup_table we can't properly detect all gaps
                // in m4a_check_sample_offset.  So just ignore decode errors till next chunk present in lookup_table 
                if (f->chunk_start)
                {
                    decode_stop();
                    return CODEC_ERROR;
                }
            }

            /* Advance codec buffer (no need to call set_offset because of this) */
            ci->advance_buffer(f->info.bytesconsumed);

            /* Output the audio */
            frame_samples = f->info.samples >> 1;

            if (empty_first_frame)
            {
                /* Remove the first frame from lead_trim, under the assumption
                 * that it had the same size as this frame
                 */
                empty_first_frame = false;
                lead_trim -= frame_samples;

                if (lead_trim < 0)
                {
                    lead_trim = 0;
                }
            }

            /* Gather number of samples for the decoded frame. */
            framelength = frame_samples - lead_trim;

            if (i == demux_res.num_sample_byte_sizes - 1)
            {
                // Size of the last frame
                // Currently ignored if time_to_sample table fails to fit.
                const uint32_t sample_duration = (demux_res.num_time_to_samples > 0 && demux_res.time_to_sample) ?
                    demux_res.time_to_sample[demux_res.num_time_to_samples - 1].sample_duration :
                    frame_samples;

                /* Currently limited to at most one frame of tail_trim.
                 * Seems to be enough.
                 */
                if (ci->id3->tail_trim == 0 && sample_duration < frame_samples)
                {
                    /* Subtract lead_trim just in case we decode a file with only
                     * one audio frame with actual data (lead_trim is usually zero
                     * here).
                     */
                    framelength = sample_duration - lead_trim;
                }
                else
                {
                    framelength -= ci->id3->tail_trim;
                }
            }

            if (framelength > 0)
            {
                ci->pcmbuf_insert(&f->out[0][lead_trim],
                                  &f->out[1][lead_trim],
                                  framelength);
                sound_samples_done += framelength;
                /* Update the elapsed-time indicator */
                elapsed_time = ((uint64_t) sound_samples_done * 1000) /
                    ci->id3->frequency;
                ci->set_elapsed(elapsed_time);
            }

            if (lead_trim > 0)
            {
                /* frame_info.samples can be 0 for frame 0. We still want to
                 * remove it from lead_trim, so do that during frame 1.
                 */
                if (0 == i && 0 == f->info.samples)
                {
                    empty_first_frame = true;
                }

                lead_trim -= frame_samples;

                if (lead_trim < 0)
                {
                    lead_trim = 0;
                }
            }

            ++i;
            decode_taken();
        }

        ci->yield();
    }

    decode_stop();

    LOGF("AAC: Decoded %lu samples\n", (unsigned long)sound_samples_done);
    return CODEC_OK;
}
