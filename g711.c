#include "g711.h"

/* ITU-T G.711 A-law companding, transcribed from spandsp's g711.h so the output
 * matches the spandsp build bit-for-bit. See John C. Bellamy, "Digital
 * Telephony", for the underlying segment/quantization layout. */

#define G711_ALAW_AMI_MASK 0x55     /* A-law alternate-mark-inversion mask */

/* Index of the most significant set bit (0-based), or -1 if none. */
static int top_bit(unsigned int bits)
{
    return bits ? 31 - __builtin_clz(bits) : -1;
}

uint8_t linear_to_alaw(int linear)
{
    int mask;
    int seg;

    if (linear >= 0) {
        /* Sign (bit 7) = 1 */
        mask = G711_ALAW_AMI_MASK | 0x80;
    } else {
        /* Sign (bit 7) = 0 */
        mask = G711_ALAW_AMI_MASK;
        linear = -linear - 1;
    }

    /* Convert the scaled magnitude to a segment number. */
    seg = top_bit(linear | 0xFF) - 7;
    if (seg >= 8) {
        if (linear >= 0)
            return (uint8_t) (0x7F ^ mask);     /* out of range: clamp to max */
        return (uint8_t) (0x00 ^ mask);         /* just below zero */
    }
    /* Combine the sign, segment, and quantization bits. */
    return (uint8_t) (((seg << 4) |
                       ((linear >> ((seg) ? (seg + 3) : 4)) & 0x0F)) ^ mask);
}

int16_t alaw_to_linear(uint8_t alaw)
{
    int i;
    int seg;

    alaw ^= G711_ALAW_AMI_MASK;
    i = ((alaw & 0x0F) << 4);
    seg = (((int) alaw & 0x70) >> 4);
    if (seg)
        i = (i + 0x108) << (seg - 1);
    else
        i += 8;
    return (int16_t) ((alaw & 0x80) ? i : -i);
}
