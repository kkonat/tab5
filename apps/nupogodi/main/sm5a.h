/*
 * Sharp SM5A - the 4-bit MCU inside a Game & Watch, and inside the Soviet
 * clones of one.
 *
 * VENDORED. This is MAME's SM5A core, by way of bzhxx's LCD-Game-Emulator and
 * artyomsoft's Raspberry Pi Pico port of it, reduced to the SM5A path and
 * wrapped in the interface below. See sm5a.c for the licence and the full
 * provenance. Fixes belong upstream first; what is different here is listed at
 * the top of that file, and the list is meant to stay short.
 *
 * The part has no bus in the ordinary sense. Its whole outside world is four
 * signals, and this header is those four plus a way to look at the LCD:
 *
 *   R      four output pins. R1 is wired to the piezo and R2-R4 select which
 *          row of the key matrix K reads, so sound and input come out of the
 *          same nibble - which is why write_r is also where the sound is.
 *   K      four inputs, read through whichever row R selected.
 *   BA, B  two more inputs, read by their own test instructions. On this
 *          family BA is a factory-test pad; on Nu, pogodi! grounding it is
 *          the infinite-lives cheat.
 *   O      the LCD drivers, 9 pins x 4 segments x 2 commons = 72 segments.
 *
 * There is one CPU, so there is one instance and the state is file-static.
 * Wanting two of these would mean the emulator was being asked to be
 * something it is not.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** O pins, and the segments they drive. Fixed by the part, not by the game. */
#define SM5A_O_PINS   9
#define SM5A_SEGMENTS (SM5A_O_PINS * 8)

/**
 * The clock, in Hz. A 32.768 kHz watch crystal, because this is a watch.
 *
 * Everything else in the emulator is derived from it: the CPU retires one
 * instruction every two ticks, and the piezo is sampled once per tick - so
 * this is also the sample rate of the sound coming out of write_r.
 */
#define SM5A_CLOCK_HZ 32768

/** Two ticks per instruction, so an instruction is two piezo samples. */
#define SM5A_CLK_DIV  2

/**
 * The four signals, supplied by whatever is pretending to be the rest of the
 * handheld. None may be NULL.
 */
typedef struct {
    /** The mask ROM. Never freed by the core; it only ever reads. */
    const uint8_t *program;

    /**
     * The key matrix. @p r_out is the R nibble already inverted the way the
     * pins are, so it is the row select as the hardware sees it. Return the
     * four K lines, active high.
     */
    uint8_t (*read_k)(uint8_t r_out);

    /** The BA pin. Non-zero makes the TAL instruction skip. */
    uint8_t (*read_ba)(void);

    /** The B pin. Non-zero makes the TB instruction skip. */
    uint8_t (*read_b)(void);

    /**
     * R changed, or a clock tick went by with it unchanged.
     *
     * Called once per clock tick - SM5A_CLOCK_HZ times a second of emulated
     * time - whether or not anything moved, which is what makes bit 0 of
     * @p r_out a 32768 Hz one-bit recording of the piezo. Sampling it only on
     * edges would be cheaper and would lose the timing that is the entire
     * sound.
     *
     * This is the hottest call in the emulator by two orders of magnitude.
     * Keep it to a store and a pointer bump.
     */
    void (*write_r)(uint8_t r_out);
} sm5a_bus_t;

/**
 * Point the core at a machine and cold-start it.
 *
 * Equivalent to the ACL pad, which is what a battery change does: RAM is
 * cleared and the program starts at its reset vector. @p bus is borrowed, not
 * copied, so it must outlive the core - a file-static in the caller.
 */
void sm5a_reset(const sm5a_bus_t *bus);

/**
 * Retire @p instructions instructions, calling write_r twice per instruction.
 *
 * The unit is instructions rather than clocks because that is what the core
 * counts; multiply by SM5A_CLK_DIV for ticks, and divide SM5A_CLOCK_HZ by
 * SM5A_CLK_DIV for how many of them a second is worth.
 */
void sm5a_run(int instructions);

/**
 * Tell the core whether any key is down.
 *
 * Separate from read_k because it is asked a different question: read_k says
 * which keys, this says whether the part may come out of halt at all. A
 * CEND'd SM5A is stopped until either a key goes down or the divider rolls
 * over, and it wakes on the fact of a keypress before anything looks at which
 * one it was.
 */
void sm5a_keys_active(bool any_down);

/**
 * Write one nibble of the part's RAM.
 *
 * Reaching into a running program's memory, which needs a reason. The reason
 * is the clock: these machines were watches first, and the ROM keeps the time
 * as six BCD nibbles that it advances off the same divider that makes the
 * game run. There is no way to *set* it from outside except through the
 * buttons, at a minute a press. The addresses are in the artwork file, put
 * there by whoever worked out where the ROM keeps them.
 *
 * Out-of-range addresses are ignored, so a wrong number in an asset file is a
 * clock that does not get set rather than a corrupted game.
 */
void sm5a_poke(uint8_t addr, uint8_t nibble);

/** Read one nibble back, so that a seeded clock can be checked rather than hoped at. */
uint8_t sm5a_peek(uint8_t addr);

/** True while the part is stopped at a CEND, waiting for a key or the second. */
bool sm5a_halted(void);

/** The program counter, for working out where a wedged ROM is wedged. */
uint16_t sm5a_pc(void);

/**
 * Whether segment @p index is lit, 0 to SM5A_SEGMENTS-1.
 *
 * The index is the one MAME's artwork uses, so it is also the one the SVG
 * labels its shapes with: 8*O + 2*segment + common, for O pin 0-8, segment
 * 0-3 and common H1/H2. Everything goes dark when the BP driver is off,
 * which is how the game blanks the display rather than by clearing the
 * segment latches.
 */
bool sm5a_segment(int index);

/**
 * How many opcodes the core did not recognise, and the last one.
 *
 * A well-dumped ROM never trips this. A non-zero count means either the file
 * is not what it claims or the core is missing an instruction, and both are
 * worth saying out loud rather than emulating around.
 */
int      sm5a_illegal_count(void);
uint16_t sm5a_illegal_op(void);
uint16_t sm5a_illegal_pc(void);
