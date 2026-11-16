/***************************************************************************
 * ARM Stack Unwinder, Michael.McTernan.2001@cs.bris.ac.uk
 *
 * This program is PUBLIC DOMAIN.
 * This means that there is no copyright and anyone is able to take a copy
 * for free and use it as they wish, with or without modifications, and in
 * any context, commercially or otherwise. The only limitation is that I
 * don't guarantee that the software is fit for any purpose or accept any
 * liability for it's use or misuse - this software is without warranty.
 ***************************************************************************
 * File Description: Unwinder client that reads local memory.
 *   This client reads from local memory and is designed to run on target
 *   along with the unwinder.  Memory read requests are implemented by
 *   casting a point to read the memory directly, although checks for
 *   alignment should probably also be made if this is to be used in
 *   production code, as otherwise the ARM may return the memory in a
 *   rotated/rolled format, or the MMU may generate an alignment exception
 *   if present and so configured.
 **************************************************************************/

/***************************************************************************
 * Includes
 ***************************************************************************/

#include "backtrace.h"
#include "safe_read.h"
#include "panic.h"

/* Each frame's return address goes through the report, and the unwinder
 * nests a call deeper (136 bytes) at each switch between ARM and Thumb: a
 * corrupt stack could go on until the crash stack overflowed. So the
 * unwinds stop at the screen's last row, or the record's last slot, and,
 * in a build without Thumb code, after an address that would switch. A
 * switch through a bx from a register not loaded from the stack isn't
 * reported and remains: nested at most once per report. */
#ifdef USE_THUMB
#define CORRUPT(address) false
#else
#define CORRUPT(address) ((address) & 1)
#endif

/***************************************************************************
 * Prototypes
 ***************************************************************************/

static Boolean CliReport(void *data, Int32 address);
static Boolean CliReadW(Int32 a, Int32 *v);
static Boolean CliReadH(Int32 a, Int16 *v);
static Boolean CliReadB(Int32 a, Int8  *v);

/***************************************************************************
 * Variables
 ***************************************************************************/

/* Table of function pointers for passing to the unwinder */
const UnwindCallbacks cliCallbacks =
    {
        CliReport,
        CliReadW,
        CliReadH,
        CliReadB
#if defined(UNW_DEBUG)
       ,printf
#endif
    };


/***************************************************************************
 * Callbacks
 ***************************************************************************/

/***************************************************************************
 *
 * Function:     CliReport
 *
 * Parameters:   data    - Pointer to data passed to UnwindStart()
 *               address - The return address of a stack frame.
 *
 * Returns:      TRUE if unwinding should continue, otherwise FALSE to
 *                 indicate that unwinding should stop.
 *
 * Description:  This function is called from the unwinder each time a stack
 *                 frame has been unwound.  The LSB of address indicates if
 *                 the processor is in ARM mode (LSB clear) or Thumb (LSB
 *                 set).
 *
 ***************************************************************************/
static Boolean CliReport(void *data, Int32 address)
{
    /* CliStack *s = (CliStack *)data; */
    unsigned *line = (unsigned *)data;
    const unsigned last = panic_last_row();

    if (*line >= last)
        return FALSE;
    lcd_putsf(0, (*line)++, "    %c: %08x",
              (address & 0x1) ? 'T' : 'A',
              address & (~0x1));
    lcd_update();

    return *line < last && !CORRUPT(address) ? TRUE : FALSE;
}

static Boolean CliReadW(const Int32 a, Int32 *v)
{
    return safe_read32((uint32_t *)a, (uint32_t *)v);
}

static Boolean CliReadH(const Int32 a, Int16 *v)
{
    return safe_read16((void *)a, (uint16_t *)v);
}

static Boolean CliReadB(const Int32 a, Int8 *v)
{
    return safe_read8((void *)a, (uint8_t *)v);
}

Boolean CliInvalidateW(const Int32 a)
{
    *(Int32 *)a = 0xdeadbeef;
    return TRUE;
}

/* rb_backtrace_collect()'s: the return addresses kept, not drawn */
struct collect
{
    unsigned int *out;
    int n, max;
};

static Boolean CollectReport(void *data, Int32 address)
{
    struct collect *c = (struct collect *)data;

    c->out[c->n++] = address;
    return c->n < c->max && !CORRUPT(address) ? TRUE : FALSE;
}

static const UnwindCallbacks collectCallbacks =
    {
        CollectReport,
        CliReadW,
        CliReadH,
        CliReadB
#if defined(UNW_DEBUG)
       ,printf
#endif
    };

int rb_backtrace_collect(int pcAddr, int spAddr, unsigned int *out, int max)
{
    struct collect c = { out, 0, max };

    if (max > 0)
        UnwindStart(pcAddr, spAddr, &collectCallbacks, &c);
    return c.n;
}

void rb_backtrace(int pcAddr, int spAddr, unsigned *line)
{
    lcd_putsf(0, (*line)++, "pc:%08x", pcAddr); 
    lcd_putsf(0, (*line)++, "sp:%08x", spAddr);
    lcd_update();

    UnwindStart(pcAddr, spAddr, &cliCallbacks, (void *)line);

    lcd_puts(0, (*line)++, "bt end");
    lcd_update();
}

/* END OF FILE */
