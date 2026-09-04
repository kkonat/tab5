// license:BSD-3-Clause
// copyright-holders:hap, Igor
/*
 * Sharp SM5A MCU core.
 *
 * VENDORED - see the provenance below before editing.
 *
 *   Originally MAME's src/devices/cpu/sm510/sm5acore.cpp and its siblings, by
 *   hap and Igor, BSD-3-Clause. Reworked into portable C by bzhxx for
 *   LCD-Game-Emulator (github.com/bzhxx/LCD-Game-Emulator), whose files under
 *   src/cpus keep the BSD-3-Clause notice above even though the project as a
 *   whole is GPLv3 - it is those, and only those, that are reused here. Flattened
 *   to the SM5A path alone by artyomsoft for pico-nu-pogodi
 *   (github.com/artyomsoft/pico-nu-pogodi), which is the shape this follows.
 *
 * What is different here, and why. Keep this list short, and prefer fixing
 * something upstream to lengthening it:
 *
 *   - one instance, so the state is file-static and every function that is not
 *     in sm5a.h is too. There is one MCU in a Game & Watch.
 *   - the four pins are function pointers in an sm5a_bus_t rather than extern
 *     functions the machine has to define. An app that draws the LCD should
 *     not also have to satisfy the linker.
 *   - op_illegal counts instead of printf-ing. Apps here have a syscall table,
 *     not a console, and a per-instruction printf into a UART would change the
 *     timing of the thing being debugged.
 *   - names are upstream's. m_acc and m_bl read oddly next to the rest of this
 *     repo, but they are what the datasheet and MAME both call them, and a
 *     vendored file that has been renamed is one that can no longer be diffed
 *     against where it came from.
 *
 * The one genuinely surprising thing about this part, if you are reading the
 * code rather than the datasheet: the program counter does not count. It is a
 * 6-bit LFSR, so "the next instruction" is a shift and a feedback bit and the
 * addresses within a page come out in a scrambled order. That is
 * increment_pc(), and it is not a bug.
 */
#include <string.h>

#include "sm5a.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define PRG_WIDTH  11
#define DATA_WIDTH 7

#define PRG_MASK  ((1 << PRG_WIDTH) - 1)
#define DATA_MASK ((1 << DATA_WIDTH) - 1)

static const sm5a_bus_t *s_bus;

static uint8_t  ram[128];

static uint16_t m_pc, m_prev_pc, m_a;
static uint16_t m_op, m_prev_op;
static uint8_t  m_param;
static uint8_t  m_acc;
static uint8_t  m_bl, m_bm;
static uint8_t  m_c;
static uint8_t  m_cb;
static bool     m_skip;
static bool     m_rsub;
static uint8_t  m_r, m_r_out;
static bool     m_k_active;
static bool     m_halt;
static uint16_t m_div;
static bool     m_1s;
static uint8_t  m_bp;
static uint8_t  m_cn, m_mx;
static int      m_icount;

/* The LCD drivers. m_o/m_ox are the shift registers the program writes into;
   the _state copies are what is actually on the glass, latched by the
   instructions that upstream calls "update segments". */
static uint8_t  m_o[SM5A_O_PINS], m_ox[SM5A_O_PINS];
static uint8_t  m_o_state[SM5A_O_PINS], m_ox_state[SM5A_O_PINS];

/* The TRS field, fixed for this part. */
#define TRS_FIELD 1

static int      s_illegal;
static uint16_t s_illegal_op, s_illegal_pc;

/* ------------------------------------------------------------------ */
/* Memory and addressing                                               */
/* ------------------------------------------------------------------ */

static inline uint8_t read_byte_program(uint16_t addr)
{
    return s_bus->program[addr];
}

/*
 * The program counter is a 6-bit linear feedback shift register, not a
 * counter: the low six bits shift right with a feedback bit on top, and the
 * page bits above are left alone. Sequential code therefore lands on
 * non-sequential addresses, which is why a ROM dump read straight through
 * looks like nonsense.
 */
static void increment_pc(void)
{
    int feed = ((m_pc >> 1 ^ m_pc) & 1) ? 0 : 0x20;
    m_pc = feed | (m_pc >> 1 & 0x1f) | (m_pc & ~0x3f);
}

static uint8_t ram_r(void)
{
    uint8_t address = (m_bm << 4 | m_bl) & DATA_MASK;
    if (address > 0x4f) {
        address &= 0x4f;
    }
    return ram[address] & 0xf;
}

static void ram_w(uint8_t data)
{
    uint8_t address = (m_bm << 4 | m_bl) & DATA_MASK;
    if (address > 0x4f) {
        address &= 0x4f;
    }
    ram[address] = data & 0xf;
}

static void do_branch(uint8_t pu, uint8_t pm, uint8_t pl)
{
    m_pc = ((pu << 10) | (pm << 6 & 0x3c0) | (pl & 0x03f)) & PRG_MASK;
}

static inline uint8_t bitmask(uint16_t param)
{
    return 1 << (param & 3);
}

static void set_su(uint8_t su) { m_a = (m_a & ~0x3c0) | (su << 6 & 0x3c0); }
static uint8_t get_su(void)    { return m_a >> 6 & 0xf; }

/* ------------------------------------------------------------------ */
/* The LCD drivers                                                     */
/* ------------------------------------------------------------------ */

static void update_segments_state(void)
{
    for (int i = 0; i < SM5A_O_PINS; i++) {
        m_o_state[i]  = m_o[i];
        m_ox_state[i] = m_ox[i];
    }
}

static void shift_w(void)
{
    for (int i = 0; i < SM5A_O_PINS - 1; i++) {
        m_ox[i] = m_ox[i + 1];
    }
}

/*
 * The segment decoder ROM, on the die. Five bits in - the CN flag and the
 * accumulator - four segments out, or'd with the M flag when CN is clear.
 */
static uint8_t get_digit(void)
{
    static const uint8_t lut_digits[0x20] = {
        0xe, 0x0, 0xc, 0x8, 0x2, 0xa, 0xe, 0x2,
        0xe, 0xa, 0x0, 0x0, 0x2, 0xa, 0x2, 0x2,
        0xb, 0x9, 0x7, 0xf, 0xd, 0xe, 0xe, 0xb,
        0xf, 0xf, 0x4, 0x0, 0xd, 0xe, 0x4, 0x0,
    };
    return lut_digits[m_cn << 4 | m_acc] | (~m_cn & m_mx);
}

/* ------------------------------------------------------------------ */
/* Instructions                                                        */
/* ------------------------------------------------------------------ */

static void op_lb(void)
{
    m_bm = m_op & 3;
    m_bl = (m_op >> 2 & 3) | ((m_op & 0xc) ? 8 : 0);
}

static void op_incb(void) { m_bl = (m_bl + 1) & 0xf;  m_skip = (m_bl == 8); }
static void op_decb(void) { m_bl = (m_bl - 1) & 0xf;  m_skip = (m_bl == 0xf); }
static void op_sbm(void)  { m_bm |= 4; }
static void op_rbm(void)  { m_bm &= ~4; }
static void op_comcb(void){ m_cb ^= 1; }

static void op_rtn0(void)
{
    update_segments_state();
    m_pc = m_a & PRG_MASK;
    m_rsub = false;
}

static void op_rtn1(void) { op_rtn0(); m_skip = true; }
static void op_ssr(void)  { set_su(m_op & 0xf); }

static void op_tr(void)
{
    m_pc = (m_pc & ~0x3f) | (m_op & 0x3f);
    if (!m_rsub) {
        do_branch(m_cb, get_su(), m_pc & 0x3f);
    }
}

static void op_trs(void)
{
    if (!m_rsub) {
        m_rsub = true;
        uint8_t su = get_su();
        m_a = m_pc;
        do_branch(TRS_FIELD, 0, m_op & 0x3f);
        if ((m_prev_op & 0xf0) == 0x70) {
            do_branch(m_cb, su, m_pc & 0x3f);
        }
    } else {
        m_pc = (m_pc & ~0xff) | (m_op << 2 & 0xc0) | (m_op & 0xf);
    }
}

static void op_exc(void)
{
    uint8_t a = m_acc;
    m_acc = ram_r();
    ram_w(a);
    m_bm ^= (m_op & 3);
}

static void op_exci(void) { op_exc(); op_incb(); }
static void op_excd(void) { op_exc(); op_decb(); }

static void op_atbp(void)
{
    m_bp = m_acc & 1;
    m_cn = m_acc >> 3 & 1;
}

static void op_ptw(void)
{
    m_o[SM5A_O_PINS - 1] = m_ox[SM5A_O_PINS - 1];
    m_o[SM5A_O_PINS - 2] = m_ox[SM5A_O_PINS - 2];
}

static void op_tw(void)
{
    for (int i = 0; i < SM5A_O_PINS; i++) {
        m_o[i] = m_ox[i];
    }
}

static void op_pdtw(void)
{
    m_ox[SM5A_O_PINS - 2] = m_ox[SM5A_O_PINS - 1];
    m_ox[SM5A_O_PINS - 1] = get_digit();
}

static void op_dtw(void) { shift_w(); m_ox[SM5A_O_PINS - 1] = get_digit(); }
static void op_wr(void)  { shift_w(); m_ox[SM5A_O_PINS - 1] = m_acc & 7; }
static void op_ws(void)  { shift_w(); m_ox[SM5A_O_PINS - 1] = m_acc | 8; }

static void op_kta(void)
{
    update_segments_state();
    m_acc = s_bus->read_k(m_r_out & 0xf) & 0xf;
}

static void op_idiv(void)  { m_div &= 0x3f; }
static void op_rmf(void)   { m_mx = 0; m_acc = 0; }
static void op_smf(void)   { m_mx = 1; }
static void op_comcn(void) { m_cn ^= 1; }

static void op_tal(void) { update_segments_state(); m_skip = (s_bus->read_ba() != 0); }
static void op_tb(void)  { update_segments_state(); m_skip = (s_bus->read_b() != 0); }

static void op_lbl(void)
{
    m_bl = m_param & 0xf;
    m_bm = (m_param & DATA_MASK) >> 4;
}

static void op_exbla(void)
{
    uint8_t a = m_acc;
    m_acc = m_bl;
    m_bl = a;
}

static void op_lda(void) { m_acc = ram_r(); m_bm ^= (m_op & 3); }

/*
 * LAX only loads when the previous instruction was not also a LAX. A run of
 * them is how the program encodes a literal wider than a nibble, and only the
 * first one may take effect.
 */
static void op_lax(void)
{
    if ((m_op & ~0xf) != (m_prev_op & ~0xf)) {
        m_acc = m_op & 0xf;
    }
}

static void op_atr(void) { m_r = m_acc; }
static void op_add(void) { m_acc = (m_acc + ram_r()) & 0xf; }

static void op_add11(void)
{
    m_acc += ram_r() + m_c;
    m_c = m_acc >> 4 & 1;
    m_skip = (m_c == 1);
    m_acc &= 0xf;
}

static void op_adx(void)
{
    m_acc += (m_op & 0xf);
    m_skip = ((m_op & 0xf) != 10 && (m_acc & 0x10) != 0);
    m_acc &= 0xf;
}

static void op_coma(void) { m_acc ^= 0xf; }
static void op_rc(void)   { m_c = 0; }
static void op_sc(void)   { m_c = 1; }
static void op_tc(void)   { m_skip = !m_c; }
static void op_tam(void)  { m_skip = (m_acc == ram_r()); }
static void op_tmi(void)  { m_skip = ((ram_r() & bitmask(m_op)) != 0); }
static void op_ta0(void)  { m_skip = !m_acc; }
static void op_tabl(void) { m_skip = (m_acc == m_bl); }

static void op_tis(void)  { m_skip = !m_1s; m_1s = false; }

static void op_rm(void)   { ram_w(ram_r() & ~bitmask(m_op)); }
static void op_sm(void)   { ram_w(ram_r() |  bitmask(m_op)); }

static void op_skip(void) { }
static void op_cend(void) { m_halt = true; }
static void op_dta(void)  { m_acc = m_div >> 11 & 0xf; }

static void op_illegal(void)
{
    s_illegal++;
    s_illegal_op = m_op;
    s_illegal_pc = m_prev_pc;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void execute_one(void)
{
    switch (m_op & 0xf0) {
    case 0x20: op_lax(); break;
    case 0x30: op_adx(); break;
    case 0x40: op_lb();  break;
    case 0x70: op_ssr(); break;

    case 0x80: case 0x90: case 0xa0: case 0xb0:
        op_tr();
        break;

    case 0xc0: case 0xd0: case 0xe0: case 0xf0:
        op_trs();
        break;

    default:
        switch (m_op & 0xfc) {
        case 0x04: op_rm();   break;
        case 0x0c: op_sm();   break;
        case 0x10: op_exc();  break;
        case 0x14: op_exci(); break;
        case 0x18: op_lda();  break;
        case 0x1c: op_excd(); break;
        case 0x54: op_tmi();  break;

        default:
            switch (m_op) {
            case 0x00: op_skip();  break;
            case 0x01: op_atr();   break;
            case 0x02: op_sbm();   break;
            case 0x03: op_atbp();  break;
            case 0x08: op_add();   break;
            case 0x09: op_add11(); break;
            case 0x0a: op_coma();  break;
            case 0x0b: op_exbla(); break;
            case 0x50: op_tal();   break;
            case 0x51: op_tb();    break;
            case 0x52: op_tc();    break;
            case 0x53: op_tam();   break;
            case 0x58: op_tis();   break;
            case 0x59: op_ptw();   break;
            case 0x5a: op_ta0();   break;
            case 0x5b: op_tabl();  break;
            case 0x5c: op_tw();    break;
            case 0x5d: op_dtw();   break;
            case 0x5f: op_lbl();   break;
            case 0x60: op_comcn(); break;
            case 0x61: op_pdtw();  break;
            case 0x62: op_wr();    break;
            case 0x63: op_ws();    break;
            case 0x64: op_incb();  break;
            case 0x65: op_idiv();  break;
            case 0x66: op_rc();    break;
            case 0x67: op_sc();    break;
            case 0x68: op_rmf();   break;
            case 0x69: op_smf();   break;
            case 0x6a: op_kta();   break;
            case 0x6b: op_rbm();   break;
            case 0x6c: op_decb();  break;
            case 0x6d: op_comcb(); break;
            case 0x6e: op_rtn0();  break;
            case 0x6f: op_rtn1();  break;

            /* 0x5e is a prefix; the byte after it picks the instruction. */
            case 0x5e:
                m_op = m_op << 8 | m_param;
                switch (m_param) {
                case 0x00: op_cend(); break;
                case 0x04: op_dta();  break;
                default:   op_illegal(); break;
                }
                break;

            default: op_illegal(); break;
            }
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The divider, and the piezo hanging off it                           */
/* ------------------------------------------------------------------ */

/*
 * R is presented to the outside once per clock tick whether it changed or
 * not, which is what makes bit 0 of it a 32768 Hz recording of the piezo
 * rather than a list of edges.
 */
static void clock_melody(void)
{
    m_r_out = (~m_r & 0xf);
    s_bus->write_r(m_r_out);
}

static void div_timer_cb(void)
{
    m_div = (m_div + 1) & 0x7fff;
    if (m_div == 0) {
        /* 32768 ticks: one second. This is the alarm clock's heartbeat, and
           also what wakes a halted part with nobody touching it. */
        m_1s = true;
        if (m_halt) {
            update_segments_state();
        }
    }
    clock_melody();
}

static void div_timer(int nb_inst)
{
    if (nb_inst > 0) {
        for (int i = 0; i < SM5A_CLK_DIV * nb_inst; i++) {
            div_timer_cb();
        }
    }
}

static void wakeup_vector(void)
{
    m_cb = 0;
    do_branch(0, 0, 0);
}

static bool wake_me_up(void)
{
    if (m_k_active || m_1s) {
        m_halt = false;
        wakeup_vector();
        return true;
    }
    return false;
}

/* The two-byte instructions. Both cost a second instruction slot. */
static void get_opcode_param(void)
{
    if (m_op == 0x5e || m_op == 0x5f) {
        m_icount--;
        m_param = read_byte_program(m_pc);
        increment_pc();
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

void sm5a_reset(const sm5a_bus_t *bus)
{
    s_bus = bus;

    memset(ram, 0, sizeof(ram));
    memset(m_o, 0, sizeof(m_o));
    memset(m_ox, 0, sizeof(m_ox));
    memset(m_o_state, 0, sizeof(m_o_state));
    memset(m_ox_state, 0, sizeof(m_ox_state));

    m_pc = m_prev_pc = 0;
    m_op = m_prev_op = 0;
    m_param = 0;
    m_acc = 0;
    m_bl = m_bm = 0;
    m_c = 0;
    m_cb = 0;
    m_cn = m_mx = 0;
    m_div = 0;
    m_k_active = false;
    m_icount = 0;

    s_illegal = 0;
    s_illegal_op = s_illegal_pc = 0;

    /* And then the reset vector, which is a jump to the top of the last page. */
    m_skip = false;
    m_halt = false;
    m_bp = 1;
    m_rsub = false;
    do_branch(0, 0xf, 0);
    m_a = m_pc;
    op_idiv();

    m_1s = true;
    m_r = 0xff;     /* pins high, so the piezo starts silent */
    m_r_out = 0;
}

void sm5a_keys_active(bool any_down)
{
    m_k_active = any_down;
}

void sm5a_run(int instructions)
{
    m_icount += instructions;

    int remaining_icount = m_icount;

    while (m_icount > 0) {
        m_icount--;

        /*
         * CEND stops the part until something wakes it. The divider still
         * runs - it is what wakes it a second later - so the time still has
         * to be accounted for, which is what makes this a run of div_timer
         * rather than an early return.
         */
        if (m_halt && !wake_me_up()) {
            div_timer(remaining_icount);
            m_icount = 0;
            return;
        }

        m_prev_op = m_op;
        m_prev_pc = m_pc;
        m_op = read_byte_program(m_pc);

        increment_pc();
        get_opcode_param();

        if (m_skip) {
            m_skip = false;
            m_op = 0;
        } else {
            execute_one();
        }

        div_timer(remaining_icount - m_icount);
        remaining_icount = m_icount;
    }
}

void sm5a_poke(uint8_t addr, uint8_t nibble)
{
    /* The same fold ram_r/ram_w do, so a poke lands where the program looks. */
    addr &= DATA_MASK;
    if (addr > 0x4f) {
        addr &= 0x4f;
    }
    ram[addr] = nibble & 0xf;
}

uint8_t sm5a_peek(uint8_t addr)
{
    addr &= DATA_MASK;
    if (addr > 0x4f) {
        addr &= 0x4f;
    }
    return ram[addr] & 0xf;
}

bool sm5a_halted(void)
{
    return m_halt;
}

uint16_t sm5a_pc(void)
{
    return m_pc;
}

bool sm5a_segment(int index)
{
    if (!m_bp || index < 0 || index >= SM5A_SEGMENTS) {
        return false;
    }
    const int o = index >> 3;
    const int y = (index >> 1) & 3;
    const int h = index & 1;

    const uint8_t s = h ? m_ox_state[o] : m_o_state[o];
    return (s >> y) & 1;
}

int      sm5a_illegal_count(void) { return s_illegal; }
uint16_t sm5a_illegal_op(void)    { return s_illegal_op; }
uint16_t sm5a_illegal_pc(void)    { return s_illegal_pc; }
