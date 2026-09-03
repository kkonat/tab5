/*
 * NeOS ABI versioning.
 *
 * Apps are relocated by name: the loader takes each undefined symbol in the
 * app's ELF, looks the string up in the firmware's syscall table, and patches
 * the address in. Nothing is resolved by index, so the table in
 * neos_syscalls.c can be reordered, regrouped and grown freely - what an app
 * depends on is the set of names, not their positions.
 *
 * That makes forward compatibility the natural state, and this header exists
 * to say exactly how far it goes.
 *
 *   MINOR is additive. New syscalls, new enumerators, new fields on a struct
 *   apps only ever hold by pointer. An app built against 1.2 keeps running on
 *   a 1.7 firmware, because every name it references is still there meaning
 *   the same thing.
 *
 *   MAJOR is everything else: a syscall removed or renamed, a signature or a
 *   meaning changed, or a new layout for a type that crosses the boundary by
 *   value. None of that can be detected at load time - an app compiled against
 *   the old layout would link cleanly and then read garbage - so a major bump
 *   retires the whole guard chain and refuses every older app outright.
 *
 * The boundary is wider than the syscall table. Anything an app's compiler
 * bakes into its binary is frozen for the life of the major version:
 *
 *   - the layout of ngl_rect_t, ngl_font_t and ngl_icon_t, which apps pass by
 *     value or read fields out of. ngl.h has _Static_asserts that trip if one
 *     of them moves.
 *   - the numeric value of an existing enumerator, and of the NGL_* / TH_*
 *     macros. Adding new ones is free; changing an old one is not. A retheme
 *     reaches only apps that are rebuilt, which is the price of a compile-time
 *     palette.
 *   - the body of every static inline in a public header. Each app carries its
 *     own copy, so fixing a bug in one does not reach apps already on the card.
 *
 * ngl_surface_t is deliberately not on that list: it is opaque to apps, so it
 * can grow at a minor version.
 */
#pragma once

#include <stdint.h>

#define NEOS_ABI_MAJOR 1
#define NEOS_ABI_MINOR 7

/*
 * Every minor of the current major, oldest first.
 *
 * The firmware defines and exports one small object per row; an app emits a
 * reference to the single row it was compiled against. So the linker and the
 * loader do the version check between them, with nothing to keep in sync by
 * hand: an app built against 1.4 fails to load on a 1.3 firmware with
 * "Can't find common neos_abi_1_4", and an app built against 1.1 resolves
 * against a 1.3 firmware because that row is still on the list.
 *
 * Bumping NEOS_ABI_MINOR means adding a row here. Forgetting to is a compile
 * error on both sides, since NEOS_ABI_SYMBOL is only declared by this list.
 *
 * A major bump replaces the list rather than extending it - drop every X(1, n)
 * and start at X(2, 0) - which is what invalidates the v1 apps on the card.
 */
#define NEOS_ABI_GUARDS(X) \
    X(1, 0)                \
    X(1, 1)                \
    X(1, 2)                \
    X(1, 3)               \
    X(1, 4)               \
    X(1, 5)               \
    X(1, 6)               \
    X(1, 7)

#define NEOS_ABI_CAT_(maj, min) neos_abi_##maj##_##min
#define NEOS_ABI_SYM_(maj, min) NEOS_ABI_CAT_(maj, min)

/** The guard object for the version this translation unit compiles against. */
#define NEOS_ABI_SYMBOL NEOS_ABI_SYM_(NEOS_ABI_MAJOR, NEOS_ABI_MINOR)

#define NEOS_ABI_DECLARE_(maj, min) extern const uint32_t NEOS_ABI_CAT_(maj, min);
NEOS_ABI_GUARDS(NEOS_ABI_DECLARE_)

#ifndef NEOS_ABI_HOST
/*
 * The app side of the check: one pointer, in every translation unit that
 * includes the API, to a symbol whose name carries the version it needs.
 *
 * `retain` is what makes it survive - apps link with --gc-sections, and this
 * pointer is by design referenced by nothing, so `used` alone (which only
 * stops the compiler discarding it) would leave the linker free to drop the
 * section and with it the whole check. scripts/abi_check.py verifies on the
 * built ELF that it is really there, because a guard that quietly vanished
 * would look exactly like a guard that passed.
 */
#if defined(__has_attribute)
#if __has_attribute(retain)
#define NEOS_ABI_KEEP_ __attribute__((used, retain))
#endif
#endif
#ifndef NEOS_ABI_KEEP_
#define NEOS_ABI_KEEP_ __attribute__((used))
#endif

static const uint32_t *const neos_abi_link_guard NEOS_ABI_KEEP_ = &NEOS_ABI_SYMBOL;
#endif /* NEOS_ABI_HOST */
