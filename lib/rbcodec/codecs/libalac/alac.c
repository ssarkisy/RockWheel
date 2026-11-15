/*
 * ALAC (Apple Lossless Audio Codec) decoder
 * Copyright (c) 2005 David Hammerton
 * All rights reserved.
 *
 * This is the actual decoder.
 *
 * http://crazney.net/programs/itunes/alac.html
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "codeclib.h"
#include "decomp.h"

#define SIGNEXTEND24(val) (((signed)val<<8)>>8)

static int16_t predictor_coef_table[32] IBSS_ATTR;
static int16_t predictor_coef_table_a[32] IBSS_ATTR;
static int16_t predictor_coef_table_b[32] IBSS_ATTR;

/* the uncompressed low bytes of a frame's samples, kept apart from the
 * output buffers which the entropy decoder writes */
static uint16_t shift_buffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE];


/* Endian/aligment safe functions - only used in alac_set_info() */
static uint32_t get_uint32be(unsigned char* p)
{
  return((p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3]);
}

static uint16_t get_uint16be(unsigned char* p)
{
  return((p[0]<<8) | p[1]);
}

void alac_set_info(alac_file *alac, char *inputbuffer)
{
  unsigned char* ptr = (unsigned char*)inputbuffer;
  ptr += 4; /* size */
  ptr += 4; /* frma */
  ptr += 4; /* alac */
  ptr += 4; /* size */
  ptr += 4; /* alac */

  ptr += 4; /* 0 ? */

  alac->setinfo_max_samples_per_frame = get_uint32be(ptr); /* buffer size / 2 ? */
  ptr += 4;
  alac->setinfo_7a = *ptr++;
  alac->setinfo_sample_size = *ptr++;
  alac->setinfo_rice_historymult = *ptr++;
  alac->setinfo_rice_initialhistory = *ptr++;
  alac->setinfo_rice_kmodifier = *ptr++;
  alac->setinfo_7f = *ptr++;

  alac->setinfo_80 = get_uint16be(ptr);
  ptr += 2;
  alac->setinfo_82 = get_uint32be(ptr);
  ptr += 4;
  alac->setinfo_86 = get_uint32be(ptr);
  ptr += 4;
  alac->setinfo_8a_rate = get_uint32be(ptr);
  ptr += 4;
}

/* stream reading */

/* supports reading 1 to 16 bits, in big endian format */
static inline uint32_t readbits_16(alac_file *alac, int bits)
{
    uint32_t result;
    int new_accumulator;

    result = (alac->input_buffer[0] << 16) |
             (alac->input_buffer[1] << 8) |
             (alac->input_buffer[2]);

    /* shift left by the number of bits we've already read,
     * so that the top 'n' bits of the 24 bits we read will
     * be the return bits */
    result = result << alac->input_buffer_bitaccumulator;

    result = result & 0x00ffffff;

    /* and then only want the top 'n' bits from that, where
     * n is 'bits' */
    result = result >> (24 - bits);

    new_accumulator = (alac->input_buffer_bitaccumulator + bits);

    /* increase the buffer pointer if we've read over n bytes. */
    alac->input_buffer += (new_accumulator >> 3);

    /* and the remainder goes back into the bit accumulator */
    alac->input_buffer_bitaccumulator = (new_accumulator & 7);

    return result;
}

/* supports reading 1 to 32 bits, in big endian format */
static inline uint32_t readbits(alac_file *alac, int bits)
{
    int32_t result = 0;

    if (bits > 16)
    {
        bits -= 16;
        result = readbits_16(alac, 16) << bits;
    }

    result |= readbits_16(alac, bits);

    return result;
}

/* reads a single bit */
static inline int readbit(alac_file *alac)
{
    int result;
    int new_accumulator;

    result = alac->input_buffer[0];

    result = result << alac->input_buffer_bitaccumulator;

    result = result >> 7 & 1;

    new_accumulator = (alac->input_buffer_bitaccumulator + 1);

    alac->input_buffer += (new_accumulator / 8);

    alac->input_buffer_bitaccumulator = (new_accumulator % 8);

    return result;
}

static inline void unreadbits(alac_file *alac, int bits)
{
    int new_accumulator = (alac->input_buffer_bitaccumulator - bits);

    alac->input_buffer += (new_accumulator >> 3);

    alac->input_buffer_bitaccumulator = (new_accumulator & 7);
    if (alac->input_buffer_bitaccumulator < 0)
        alac->input_buffer_bitaccumulator *= -1;
}

#define count_leading_zeros(x) bs_generic(x, BS_CLZ)

/* Without a CLZ instruction, bs_generic() is a table lookup and its
   16-bit form (BS_SHORT) is cheaper; use it when the argument fits. */
#if !defined(CPU_ARM) || ARM_ARCH < 5
#define CLZ_BY_TABLE
#endif

#define RICE_THRESHOLD 8 // maximum number of bits for a rice prefix.

static inline int32_t entropy_decode_value(alac_file* alac,
                             int readsamplesize,
                             int k) ICODE_ATTR_ALAC;
static inline int32_t entropy_decode_value(alac_file* alac,
                             int readsamplesize,
                             int k)
{
    int32_t x = 0; // decoded value
    
    // read x, number of 1s before 0 represent the rice value.
    while (x <= RICE_THRESHOLD && readbit(alac))
    {
        x++;
    }
    
    if (x > RICE_THRESHOLD)
    {
        // read the number from the bit stream (raw value)
        int32_t value;
        
        value = readbits(alac, readsamplesize);
        
        /* mask value to readsamplesize size */
        if (readsamplesize != 32)
            value &= (((uint32_t)0xffffffff) >> (32 - readsamplesize));
        
        x = value;
    }
    else
    {
        if (k != 1)
        {
            int extrabits = readbits(alac, k);
            
            // x = x * (2^k - 1)
            x = (x << k) - x;
            
            if (extrabits > 1)
                x += extrabits - 1;
            else
                unreadbits(alac, 1);
        }
    }
    
    return x;
}

/* After a sample that leaves the history below 128, a run of zeros may
 * follow: its length, written as a value with a parameter of its own, then
 * that many zero samples, no more than the frame has left. Returns the
 * index of the last sample the run fills, or output_count for an empty
 * run. */
static int entropy_zero_run(alac_file *alac,
                            int32_t *output_buffer,
                            int output_count,
                            int output_size,
                            int history,
                            int *sign_modifier,
                            int rice_kmodifier_mask) ICODE_ATTR_ALAC;
static int entropy_zero_run(alac_file *alac,
                            int32_t *output_buffer,
                            int output_count,
                            int output_size,
                            int history,
                            int *sign_modifier,
                            int rice_kmodifier_mask)
{
    int32_t block_size;
    int k;

    *sign_modifier = 1;

    k = count_leading_zeros(history) + ((history + 16) / 64) - 24;

    // note: block_size is always 16bit
    block_size = entropy_decode_value(alac, 16, k) & rice_kmodifier_mask;

    if (block_size > 0xFFFF)
        *sign_modifier = 0;

    // got block_size 0s, no more than the frame has left
    if (block_size > output_size - output_count - 1)
        block_size = output_size - output_count - 1;
    if (block_size > 0)
    {
        memset(&output_buffer[output_count + 1], 0,
               block_size * sizeof(*output_buffer));
        output_count += block_size;
    }

    return output_count;
}

#if defined(CPU_ARM_CLASSIC) && !defined(ALAC_NO_ASM)
#define ALAC_RICE_ASM
/* The common path of entropy_rice_decode()'s loop, in arm.S; the field
 * offsets are the assembly's */
struct alac_rice
{
    const unsigned char *buffer;  /* in */
    int index;                    /* in and out: bits past buffer */
    int history;                  /* in and out */
    int zero_run;                 /* out: a run of zeros follows */
    int kmodifier;                /* in, 1 to 17 */
    int historymult;              /* in */
};

int alac_rice_decode_arm(int32_t *out, int n, struct alac_rice *st);
#endif

static void entropy_rice_decode(alac_file* alac,
                         int32_t* output_buffer,
                         int output_size,
                         int readsamplesize,
                         int rice_initialhistory,
                         int rice_kmodifier,
                         int rice_historymult,
                         int rice_kmodifier_mask) ICODE_ATTR_ALAC;
static void entropy_rice_decode(alac_file* alac,
                         int32_t* output_buffer,
                         int output_size,
                         int readsamplesize,
                         int rice_initialhistory,
                         int rice_kmodifier,
                         int rice_historymult,
                         int rice_kmodifier_mask)
{
    int                output_count;
    int                history = rice_initialhistory;
    int                sign_modifier = 0;
    
    for (output_count = 0; output_count < output_size; output_count++)
    {
        int32_t        decoded_value;
        int32_t        final_value;
        int32_t        k;
        uint32_t       x;

#ifdef ALAC_RICE_ASM
        /* The assembly decodes every sample it can, then this loop takes
         * the one it stopped before, or the run of zeros after the last one
         * it decoded. It needs a sign modifier of 0 and a history of 0 or
         * more; a sign modifier of 0 here implies the latter. */
        if (sign_modifier == 0 && history >= 0 && rice_kmodifier <= 17)
        {
            struct alac_rice st;

            st.buffer = alac->input_buffer;
            st.index = alac->input_buffer_bitaccumulator;
            st.history = history;
            st.kmodifier = rice_kmodifier;
            st.historymult = rice_historymult;
            output_count += alac_rice_decode_arm(output_buffer + output_count,
                                                 output_size - output_count,
                                                 &st);
            alac->input_buffer += st.index >> 3;
            alac->input_buffer_bitaccumulator = st.index & 7;
            history = st.history;

            if (st.zero_run)
            {
                output_count = entropy_zero_run(alac, output_buffer,
                                                output_count - 1, output_size,
                                                history, &sign_modifier,
                                                rice_kmodifier_mask);
                history = 0;
                continue;
            }
            if (output_count >= output_size)
                break;
        }
#endif

        x = (history >> 9) + 3;

#ifdef CLZ_BY_TABLE
        if (x < 0x10000)
        {
            /* the same k as below, for 16-bit x */
            k = 31 - bs_generic(x, BS_CLZ|BS_SHORT);
            if (k > rice_kmodifier)
                k = rice_kmodifier;
        }
        else
#endif
        {
            k = 31 - rice_kmodifier - count_leading_zeros(x);
            if (k < 0) k += rice_kmodifier;
            else k = rice_kmodifier;
        }
        
        decoded_value = entropy_decode_value(alac, readsamplesize, k);
        
        decoded_value += sign_modifier;
        final_value = (decoded_value + 1) / 2; // inc by 1 and shift out sign bit
        if (decoded_value & 1) // the sign is stored in the low bit
            final_value *= -1;
        
        output_buffer[output_count] = final_value;
        
        sign_modifier = 0;
        
        // update history
        history += (decoded_value * rice_historymult)
                - ((history * rice_historymult) >> 9);
        
        if (decoded_value > 0xFFFF)
            history = 0xFFFF;
        
        // special case, for compressed blocks of 0
        if ((history < 128) && (output_count + 1 < output_size))
        {
            output_count = entropy_zero_run(alac, output_buffer, output_count,
                                            output_size, history,
                                            &sign_modifier,
                                            rice_kmodifier_mask);
            history = 0;
        }
    }
}

#define SIGN_EXTENDED32(val, bits) ((val << (32 - bits)) >> (32 - bits))

#define SIGN_ONLY(v) \
                     ((v < 0) ? (-1) : \
                                ((v > 0) ? (1) : \
                                           (0)))

#if defined(CPU_ARM_CLASSIC) && !defined(ALAC_NO_ASM)
#define ALAC_FIR_ASM
/* The adaptive filter after its warm-up at orders 1 to 8, in arm.S, with
 * the residuals read from the output buffer and the coefficients held in
 * 32 bits: see fir_asm_fits() */
void alac_fir_adapt_arm(int32_t *buffer_out, int count, int16_t *coefs,
                        int order, int quant, int readsamplesize);

/* Each coefficient moves at most one step a sample, so none can wrap at 16
 * bits in count samples if each is that far from the limit. Samples of at
 * most 31 bits keep a difference of two from wrapping, so its sign is the
 * one the C tests: the first sample, copied as it came, as well as those
 * sign-extended from readsamplesize. */
static inline int fir_asm_fits(const int16_t *coefs, int n, int count,
                               int quant, int readsamplesize, int32_t first)
{
    int j;

    if (quant < 1 || readsamplesize < 1 || readsamplesize > 31 ||
        first < -0x40000000 || first > 0x3fffffff)
        return 0;
    for (j = 0; j < n; j++)
        if (coefs[j] > 32767 - count || coefs[j] < -32768 + count)
            return 0;
    return 1;
}
#endif

static void predictor_decompress_fir_adapt(int32_t *error_buffer,
                                           int32_t *buffer_out,
                                           int output_size,
                                           int readsamplesize,
                                           int16_t *predictor_coef_table,
                                           int predictor_coef_num,
                                           int predictor_quantitization) ICODE_ATTR_ALAC;
static void predictor_decompress_fir_adapt(int32_t *error_buffer,
                                           int32_t *buffer_out,
                                           int output_size,
                                           int readsamplesize,
                                           int16_t *predictor_coef_table,
                                           int predictor_coef_num,
                                           int predictor_quantitization)
{
    int i;

    /* first sample always copies */
    *buffer_out = *error_buffer;

    if (!predictor_coef_num)
    {
        if (output_size <= 1) return;
        memcpy(buffer_out+1, error_buffer+1, (output_size-1) * 4);
        return;
    }

    if (predictor_coef_num == 0x1f) /* 11111 - max value of predictor_coef_num */
    { /* second-best case scenario for fir decompression,
       * error describes a small difference from the previous sample only
       */
        if (output_size <= 1) return;
        for (i = 0; i < output_size - 1; i++)
        {
            int32_t prev_value;
            int32_t error_value;

            prev_value = buffer_out[i];
            error_value = error_buffer[i+1];
            buffer_out[i+1] = SIGN_EXTENDED32((prev_value + error_value), readsamplesize);
        }
        return;
    }

    /* read warm-up samples */
    if (predictor_coef_num > 0)
    {
        int i;
        for (i = 0; i < predictor_coef_num; i++)
        {
            int32_t val;

            val = buffer_out[i] + error_buffer[i+1];

            val = SIGN_EXTENDED32(val, readsamplesize);

            buffer_out[i+1] = val;
        }
    }

#ifdef ALAC_FIR_ASM
    /* the orders encoders use: Apple's 4 and 8, ffmpeg's 4 to 6 by default
       (it writes up to 30 when asked; those go to the C below) */
    if (predictor_coef_num <= 8 && output_size > predictor_coef_num + 1 &&
        error_buffer == buffer_out &&
        fir_asm_fits(predictor_coef_table, predictor_coef_num,
                     output_size - predictor_coef_num - 1,
                     predictor_quantitization, readsamplesize, buffer_out[0]))
    {
        alac_fir_adapt_arm(buffer_out, output_size - predictor_coef_num - 1,
                           predictor_coef_table, predictor_coef_num,
                           predictor_quantitization, readsamplesize);
        return;
    }
    /* Only a rare frame gets here, and the general case decodes orders 4
     * and 8 exactly as the unrolled copies below do: this build compiles
     * those copies out */
#else
    /* 4 and 8 are very common cases (the only ones i've seen).

      The following code is an initial attempt to unroll and optimise
      these two cases by the Rockbox project.  More work is needed. 
     */

    /* optimised case: 4 */
    if (predictor_coef_num == 4)
    {
        for (i = 4 + 1; i < output_size; i++)
        {
            int sum = 0;
            int outval;
            int error_val = error_buffer[i];

            sum = (buffer_out[4] - buffer_out[0]) * predictor_coef_table[0]
                + (buffer_out[3] - buffer_out[0]) * predictor_coef_table[1]
                + (buffer_out[2] - buffer_out[0]) * predictor_coef_table[2]
                + (buffer_out[1] - buffer_out[0]) * predictor_coef_table[3];

            outval = (1 << (predictor_quantitization-1)) + sum;
            outval = outval >> predictor_quantitization;
            outval = outval + buffer_out[0] + error_val;
            outval = SIGN_EXTENDED32(outval, readsamplesize);

            buffer_out[4+1] = outval;

            if (error_val > 0)
            {
                int predictor_num = 4 - 1;

                while (predictor_num >= 0 && error_val > 0)
                {
                    int val = buffer_out[0] - buffer_out[4 - predictor_num];

                    if (val!=0) {
                       if (val < 0) {
                         predictor_coef_table[predictor_num]++;
                         val=-val;
                       } else {
                         predictor_coef_table[predictor_num]--;
                       }
                       error_val -= ((val >> predictor_quantitization) * (4 - predictor_num));
                    }                      
                    predictor_num--;
                }
            }
            else if (error_val < 0)
            {
                int predictor_num = 4 - 1;

                while (predictor_num >= 0 && error_val < 0)
                {
                    int val = buffer_out[0] - buffer_out[4 - predictor_num];

                    if (val != 0) {
                      if (val > 0) {
                        predictor_coef_table[predictor_num]++;
                        val=-val; /* neg value */
                      } else {
                        predictor_coef_table[predictor_num]--;
                      }
                      error_val -= ((val >> predictor_quantitization) * (4 - predictor_num));
                    }
                    predictor_num--;
                }
            }

            buffer_out++;
        }
        return;
    }

    /* optimised case: 8 */
    if (predictor_coef_num == 8)
    {
        for (i = 8 + 1;
             i < output_size;
             i++)
        {
            int sum;
            int outval;
            int error_val = error_buffer[i];

            sum = (buffer_out[8] - buffer_out[0]) * predictor_coef_table[0]
                + (buffer_out[7] - buffer_out[0]) * predictor_coef_table[1]
                + (buffer_out[6] - buffer_out[0]) * predictor_coef_table[2]
                + (buffer_out[5] - buffer_out[0]) * predictor_coef_table[3]
                + (buffer_out[4] - buffer_out[0]) * predictor_coef_table[4]
                + (buffer_out[3] - buffer_out[0]) * predictor_coef_table[5]
                + (buffer_out[2] - buffer_out[0]) * predictor_coef_table[6]
                + (buffer_out[1] - buffer_out[0]) * predictor_coef_table[7];

            outval = (1 << (predictor_quantitization-1)) + sum;
            outval = outval >> predictor_quantitization;
            outval = outval + buffer_out[0] + error_val;
            outval = SIGN_EXTENDED32(outval, readsamplesize);

            buffer_out[8+1] = outval;

            if (error_val > 0)
            {
                int predictor_num = 8 - 1;

                while (predictor_num >= 0 && error_val > 0)
                {
                    int val = buffer_out[0] - buffer_out[8 - predictor_num];

                    if (val!=0) {
                       if (val < 0) {
                         predictor_coef_table[predictor_num]++;
                         val=-val;
                       } else {
                         predictor_coef_table[predictor_num]--;
                       }
                       error_val -= ((val >> predictor_quantitization) * (8 - predictor_num));
                    }                      
                    predictor_num--;
                }
            }
            else if (error_val < 0)
            {
                int predictor_num = 8 - 1;

                while (predictor_num >= 0 && error_val < 0)
                {
                    int val = buffer_out[0] - buffer_out[8 - predictor_num];
                    if (val != 0) {
                      if (val > 0) {
                        predictor_coef_table[predictor_num]++;
                        val=-val; /* neg value */
                      } else {
                        predictor_coef_table[predictor_num]--;
                      }
                      error_val -= ((val >> predictor_quantitization) * (8 - predictor_num));
                    }
                    predictor_num--;
                }
            }

            buffer_out++;
        }
        return;
    } 
#endif /* ALAC_FIR_ASM */

    /* general case */
    if (predictor_coef_num > 0)
    {
        for (i = predictor_coef_num + 1;
             i < output_size;
             i++)
        {
            int j;
            int sum = 0;
            int outval;
            int error_val = error_buffer[i];

            for (j = 0; j < predictor_coef_num; j++)
            {
                sum += (buffer_out[predictor_coef_num-j] - buffer_out[0]) *
                       predictor_coef_table[j];
            }

            outval = (1 << (predictor_quantitization-1)) + sum;
            outval = outval >> predictor_quantitization;
            outval = outval + buffer_out[0] + error_val;
            outval = SIGN_EXTENDED32(outval, readsamplesize);

            buffer_out[predictor_coef_num+1] = outval;

            if (error_val > 0)
            {
                int predictor_num = predictor_coef_num - 1;

                while (predictor_num >= 0 && error_val > 0)
                {
                    int val = buffer_out[0] - buffer_out[predictor_coef_num - predictor_num];
                    int sign = SIGN_ONLY(val);

                    predictor_coef_table[predictor_num] -= sign;

                    val *= sign; /* absolute value */

                    error_val -= ((val >> predictor_quantitization) *
                                  (predictor_coef_num - predictor_num));

                    predictor_num--;
                }
            }
            else if (error_val < 0)
            {
                int predictor_num = predictor_coef_num - 1;

                while (predictor_num >= 0 && error_val < 0)
                {
                    int val = buffer_out[0] - buffer_out[predictor_coef_num - predictor_num];
                    int sign = - SIGN_ONLY(val);

                    predictor_coef_table[predictor_num] -= sign;

                    val *= sign; /* neg value */

                    error_val -= ((val >> predictor_quantitization) *
                                  (predictor_coef_num - predictor_num));

                    predictor_num--;
                }
            }

            buffer_out++;
        }
    }
}

static void deinterlace_16(int32_t* buffer0,
                           int32_t* buffer1,
                           int numsamples,
                           uint8_t interlacing_shift,
                           uint8_t interlacing_leftweight) ICODE_ATTR_ALAC;
static void deinterlace_16(int32_t* buffer0,
                           int32_t* buffer1,
                           int numsamples,
                           uint8_t interlacing_shift,
                           uint8_t interlacing_leftweight)
{
    int i;
    if (numsamples <= 0) return;

    /* weighted interlacing */
    if (interlacing_leftweight)
    {
        for (i = 0; i < numsamples; i++)
        {
            int32_t difference, midright;

            midright = buffer0[i];
            difference = buffer1[i];

            buffer0[i] = ((midright - ((difference * interlacing_leftweight)
                            >> interlacing_shift)) + difference) << SCALE16;
            buffer1[i] = (midright - ((difference * interlacing_leftweight) 
                            >> interlacing_shift)) << SCALE16;
        }

        return;
    }

    /* otherwise basic interlacing took place */
    for (i = 0; i < numsamples; i++)
    {
        buffer0[i] = buffer0[i] << SCALE16;
        buffer1[i] = buffer1[i] << SCALE16;
    }
}

static void deinterlace_24(int32_t *buffer0, int32_t *buffer1,
                           int uncompressed_bytes,
                           uint16_t *uncompressed_bytes_buffer0,
                           uint16_t *uncompressed_bytes_buffer1,
                           int numsamples,
                           uint8_t interlacing_shift,
                           uint8_t interlacing_leftweight) ICODE_ATTR_ALAC;
static void deinterlace_24(int32_t *buffer0, int32_t *buffer1,
                           int uncompressed_bytes,
                           uint16_t *uncompressed_bytes_buffer0,
                           uint16_t *uncompressed_bytes_buffer1,
                           int numsamples,
                           uint8_t interlacing_shift,
                           uint8_t interlacing_leftweight)
{
    int i;
    if (numsamples <= 0) return;
    
    /* weighted interlacing */
    if (interlacing_leftweight)
    {
        for (i = 0; i < numsamples; i++)
        {
            int32_t difference, midright;
            
            midright = buffer0[i];
            difference = buffer1[i];
            
            buffer0[i] = (midright - ((difference * interlacing_leftweight)
                            >> interlacing_shift)) + difference;
            buffer1[i] = midright - ((difference * interlacing_leftweight)
                            >> interlacing_shift);
            
            if (uncompressed_bytes)
            {
                uint32_t mask = ~(0xFFFFFFFF << (uncompressed_bytes * 8));
                buffer0[i] <<= (uncompressed_bytes * 8);
                buffer1[i] <<= (uncompressed_bytes * 8);
                
                buffer0[i] |= uncompressed_bytes_buffer0[i] & mask;
                buffer1[i] |= uncompressed_bytes_buffer1[i] & mask;
            }

            buffer0[i] = buffer0[i] << SCALE24;
            buffer1[i] = buffer1[i] << SCALE24;
        }
        
        return;
    }
    
    /* otherwise basic interlacing took place */
    for (i = 0; i < numsamples; i++)
    {
        if (uncompressed_bytes)
        {
            uint32_t mask = ~(0xFFFFFFFF << (uncompressed_bytes * 8));
            buffer0[i] <<= (uncompressed_bytes * 8);
            buffer1[i] <<= (uncompressed_bytes * 8);
            
            buffer0[i] |= uncompressed_bytes_buffer0[i] & mask;
            buffer1[i] |= uncompressed_bytes_buffer1[i] & mask;
        }
        
        buffer0[i] = buffer0[i] << SCALE24;
        buffer1[i] = buffer1[i] << SCALE24;
    }
    
}

#if defined(CPU_ARM_CLASSIC) && !defined(ALAC_NO_ASM)
#define ALAC_D24_ASM
/* deinterlace_24() in arm.S for a stereo frame with one uncompressed byte a
 * sample, reading those bytes from the frame at lows, where they are
 * interleaved left and right, each straddling two bytes at bit offset
 * 8 - lows_shift */
void alac_deinterlace_24_arm(int32_t *buffer0, int32_t *buffer1,
                             const unsigned char *lows, int count,
                             int lows_shift, int interlacing_shift,
                             int interlacing_leftweight);
#endif

static inline int decode_frame_mono(
                    alac_file *alac,
                    int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                    void (*yield)(void))
{
    int hassize;
    int isnotcompressed;
    int readsamplesize;
    int infosamplesize = alac->setinfo_sample_size;
    int outputsamples = alac->setinfo_max_samples_per_frame;

    int uncompressed_bytes;
    int ricemodifier;


    /* 2^result = something to do with output waiting.
     * perhaps matters if we read > 1 frame in a pass?
     */
    readbits(alac, 4);

    readbits(alac, 12); /* unknown, skip 12 bits */

    hassize = readbits(alac, 1); /* the output sample size is stored soon */

    /* number of bytes in the (compressed) stream that are not compressed */
    uncompressed_bytes = readbits(alac, 2);

    isnotcompressed = readbits(alac, 1); /* whether the frame is compressed */

    if (hassize)
    {
        /* now read the number of samples,
         * as a 32bit integer */
        outputsamples = readbits(alac, 32);
    }
    /* the output buffers hold ALAC_BLOCKSIZE */
    if (outputsamples < 0 || outputsamples > ALAC_BLOCKSIZE)
        return -1;

    readsamplesize = infosamplesize - (uncompressed_bytes * 8);

    if (!isnotcompressed)
    { /* so it is compressed */
        int predictor_coef_num;
        int prediction_type;
        int prediction_quantitization;
        int i;

        /* skip 16 bits, not sure what they are. seem to be used in
         * two channel case */
        readbits(alac, 8);
        readbits(alac, 8);

        prediction_type = readbits(alac, 4);
        prediction_quantitization = readbits(alac, 4);

        ricemodifier = readbits(alac, 3);
        predictor_coef_num = readbits(alac, 5);

        /* read the predictor table */
        for (i = 0; i < predictor_coef_num; i++)
        {
            predictor_coef_table[i] = (int16_t)readbits(alac, 16);
        }

        if (uncompressed_bytes)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                shift_buffer[0][i] = readbits(alac, uncompressed_bytes * 8);
            }
        }

        yield();

        entropy_rice_decode(alac,
                            outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();

        if (prediction_type == 0)
        { /* adaptive fir */
            predictor_decompress_fir_adapt(outputbuffer[0],
                                           outputbuffer[0],
                                           outputsamples,
                                           readsamplesize,
                                           predictor_coef_table,
                                           predictor_coef_num,
                                           prediction_quantitization);
        }
        else
        {
            //fprintf(stderr, "FIXME: unhandled predicition type: %i\n", prediction_type);
            /* i think the only other prediction type (or perhaps this is just a
             * boolean?) runs adaptive fir twice.. like:
             * predictor_decompress_fir_adapt(predictor_error, tempout, ...)
             * predictor_decompress_fir_adapt(predictor_error, outputsamples ...)
             * little strange..
             */
        }

    }
    else
    { /* not compressed, easy case */
        if (infosamplesize <= 16)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits = readbits(alac, infosamplesize);

                audiobits = SIGN_EXTENDED32(audiobits, infosamplesize);

                outputbuffer[0][i] = audiobits;
            }
        }
        else
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits;

                audiobits = readbits(alac, 16);
                /* special case of sign extension..
                 * as we'll be ORing the low 16bits into this */
                audiobits = audiobits << (infosamplesize - 16);
                audiobits |= readbits(alac, infosamplesize - 16);
                audiobits = SIGNEXTEND24(audiobits);

                outputbuffer[0][i] = audiobits;
            }
        }
        uncompressed_bytes = 0; // always 0 for uncompressed
    }

    yield();

    switch(infosamplesize)
    {
    case 16:
    {
        int i;
        for (i = 0; i < outputsamples; i++)
        {
            /* Output mono data as stereo */
            outputbuffer[0][i] = outputbuffer[0][i] << SCALE16;
            outputbuffer[1][i] = outputbuffer[0][i];
        }
        break;
    }
    case 24:
    {
        int i;
        for (i = 0; i < outputsamples; i++)
        {
            int32_t sample = outputbuffer[0][i];
            
            if (uncompressed_bytes)
            {
                uint32_t mask;
                sample = sample << (uncompressed_bytes * 8);
                mask = ~(0xFFFFFFFF << (uncompressed_bytes * 8));
                sample |= shift_buffer[0][i] & mask;
            }
            
            outputbuffer[0][i] = sample << SCALE24;
            outputbuffer[1][i] = outputbuffer[0][i];
        }
        break;
    }
    case 20:
    case 32:
        //fprintf(stderr, "FIXME: unimplemented sample size %i\n", infosamplesize);
        break;
    default:
        break;
    }

    return outputsamples;
}

static inline int decode_frame_stereo(
                    alac_file *alac,
                    int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                    void (*yield)(void))
{
    int hassize;
    int isnotcompressed;
    int readsamplesize;
    int infosamplesize = alac->setinfo_sample_size;
    int outputsamples = alac->setinfo_max_samples_per_frame;
    int uncompressed_bytes;
#ifdef ALAC_D24_ASM
    const unsigned char *lows = NULL;
    int lows_shift = 0;
#endif

    uint8_t interlacing_shift;
    uint8_t interlacing_leftweight;

    /* 2^result = something to do with output waiting.
     * perhaps matters if we read > 1 frame in a pass?
     */
    readbits(alac, 4);

    readbits(alac, 12); /* unknown, skip 12 bits */

    hassize = readbits(alac, 1); /* the output sample size is stored soon */

    /* the number of bytes in the (compressed) stream that are not compressed */
    uncompressed_bytes = readbits(alac, 2);

    isnotcompressed = readbits(alac, 1); /* whether the frame is compressed */

    if (hassize)
    {
        /* now read the number of samples,
         * as a 32bit integer */
        outputsamples = readbits(alac, 32);
    }
    /* the output buffers hold ALAC_BLOCKSIZE */
    if (outputsamples < 0 || outputsamples > ALAC_BLOCKSIZE)
        return -1;

    readsamplesize = infosamplesize - (uncompressed_bytes * 8) + 1;

    yield();
    if (!isnotcompressed)
    { /* compressed */
        int predictor_coef_num_a;
        int prediction_type_a;
        int prediction_quantitization_a;
        int ricemodifier_a;

        int predictor_coef_num_b;
        int prediction_type_b;
        int prediction_quantitization_b;
        int ricemodifier_b;

        int i;

        interlacing_shift = readbits(alac, 8);
        interlacing_leftweight = readbits(alac, 8);

        /******** channel 1 ***********/
        prediction_type_a = readbits(alac, 4);
        prediction_quantitization_a = readbits(alac, 4);

        ricemodifier_a = readbits(alac, 3);
        predictor_coef_num_a = readbits(alac, 5);

        /* read the predictor table */
        for (i = 0; i < predictor_coef_num_a; i++)
        {
            predictor_coef_table_a[i] = (int16_t)readbits(alac, 16);
        }

        /******** channel 2 *********/
        prediction_type_b = readbits(alac, 4);
        prediction_quantitization_b = readbits(alac, 4);

        ricemodifier_b = readbits(alac, 3);
        predictor_coef_num_b = readbits(alac, 5);

        /* read the predictor table */
        for (i = 0; i < predictor_coef_num_b; i++)
        {
            predictor_coef_table_b[i] = (int16_t)readbits(alac, 16);
        }

        /*********************/
#ifdef ALAC_D24_ASM
        if (uncompressed_bytes == 1 && infosamplesize == 24)
        { /* the merge reads them where they are */
            lows = alac->input_buffer;
            lows_shift = 8 - alac->input_buffer_bitaccumulator;
            alac->input_buffer += 2 * outputsamples;
        }
        else
#endif
        if (uncompressed_bytes)
        { /* see mono case */
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                shift_buffer[0][i] = readbits(alac, uncompressed_bytes * 8);
                shift_buffer[1][i] = readbits(alac, uncompressed_bytes * 8);
            }
        }

        yield();
        /* channel 1 */
        entropy_rice_decode(alac,
                            outputbuffer[0],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier_a * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();
        if (prediction_type_a == 0)
        { /* adaptive fir */
            predictor_decompress_fir_adapt(outputbuffer[0],
                                           outputbuffer[0],
                                           outputsamples,
                                           readsamplesize,
                                           predictor_coef_table_a,
                                           predictor_coef_num_a,
                                           prediction_quantitization_a);
        }
        else
        { /* see mono case */
            //fprintf(stderr, "FIXME: unhandled predicition type: %i\n", prediction_type_a);
        }

        yield();

        /* channel 2 */
        entropy_rice_decode(alac,
                            outputbuffer[1],
                            outputsamples,
                            readsamplesize,
                            alac->setinfo_rice_initialhistory,
                            alac->setinfo_rice_kmodifier,
                            ricemodifier_b * alac->setinfo_rice_historymult / 4,
                            (1 << alac->setinfo_rice_kmodifier) - 1);

        yield();
        if (prediction_type_b == 0)
        { /* adaptive fir */
            predictor_decompress_fir_adapt(outputbuffer[1],
                                           outputbuffer[1],
                                           outputsamples,
                                           readsamplesize,
                                           predictor_coef_table_b,
                                           predictor_coef_num_b,
                                           prediction_quantitization_b);
        }
        else
        {
            //fprintf(stderr, "FIXME: unhandled predicition type: %i\n", prediction_type_b);
        }
    }
    else
    { /* not compressed, easy case */
        if (infosamplesize <= 16)
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits_a, audiobits_b;

                audiobits_a = readbits(alac, infosamplesize);
                audiobits_b = readbits(alac, infosamplesize);

                audiobits_a = SIGN_EXTENDED32(audiobits_a, infosamplesize);
                audiobits_b = SIGN_EXTENDED32(audiobits_b, infosamplesize);

                outputbuffer[0][i] = audiobits_a;
                outputbuffer[1][i] = audiobits_b;
            }
        }
        else
        {
            int i;
            for (i = 0; i < outputsamples; i++)
            {
                int32_t audiobits_a, audiobits_b;

                audiobits_a = readbits(alac, 16);
                audiobits_a = audiobits_a << (infosamplesize - 16);
                audiobits_a |= readbits(alac, infosamplesize - 16);
                audiobits_a = SIGNEXTEND24(audiobits_a);

                audiobits_b = readbits(alac, 16);
                audiobits_b = audiobits_b << (infosamplesize - 16);
                audiobits_b |= readbits(alac, infosamplesize - 16);
                audiobits_b = SIGNEXTEND24(audiobits_b);

                outputbuffer[0][i] = audiobits_a;
                outputbuffer[1][i] = audiobits_b;
            }
        }
        uncompressed_bytes = 0; // always 0 for uncompressed
        interlacing_shift = 0;
        interlacing_leftweight = 0;
    }

    yield();

    switch(infosamplesize)
    {
    case 16:
    {
        deinterlace_16(outputbuffer[0],
                       outputbuffer[1],
                       outputsamples,
                       interlacing_shift,
                       interlacing_leftweight);
        break;
    }
    case 24:
    {
#ifdef ALAC_D24_ASM
        if (lows)
        {
            alac_deinterlace_24_arm(outputbuffer[0], outputbuffer[1], lows,
                                    outputsamples, lows_shift,
                                    interlacing_shift, interlacing_leftweight);
            break;
        }
#endif
        deinterlace_24(outputbuffer[0],
                       outputbuffer[1],
                       uncompressed_bytes,
                       shift_buffer[0],
                       shift_buffer[1],
                       outputsamples,
                       interlacing_shift,
                       interlacing_leftweight);            
        break;
    }
    case 20:
    case 32:
        //fprintf(stderr, "FIXME: unimplemented sample size %i\n", infosamplesize);
        break;
    default:
        break;
    }
    return outputsamples;
}

int alac_decode_frame(alac_file *alac,
                      unsigned char *inbuffer,
                      int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE],
                      void (*yield)(void))
{
    int channels;
    int outputsamples;
    unsigned char *input_buffer_start;

    /* the rice limit sizes reads and shifts in the entropy decoder;
     * encoders write 14 */
    if (alac->setinfo_rice_kmodifier < 1 || alac->setinfo_rice_kmodifier > 25)
        return -1;

    /* setup the stream */
    alac->input_buffer = inbuffer;
    alac->input_buffer_bitaccumulator = 0;
    
    /* save to gather byte consumption */
    input_buffer_start = alac->input_buffer;

    channels = readbits(alac, 3);

    /* TODO: The mono and stereo functions should be combined. */
    switch(channels)
    {
        case 0: /* 1 channel */
            outputsamples=decode_frame_mono(alac,outputbuffer,yield);
            break;
        case 1: /* 2 channels */
            outputsamples=decode_frame_stereo(alac,outputbuffer,yield);
            break;
        default: /* Unsupported */
            return -1;
    }
    
    /* calculate consumed bytes */
    alac->bytes_consumed = (int)(alac->input_buffer - input_buffer_start);
    alac->bytes_consumed += (alac->input_buffer_bitaccumulator>5) ? 2 : 1;

    return outputsamples;
}

/* rockbox: not used
void create_alac(int samplesize, int numchannels, alac_file* alac)
{
    alac->samplesize = samplesize;
    alac->numchannels = numchannels;
    alac->bytespersample = (samplesize / 8) * numchannels;
} */
