/*
 * The OS ABI exported to loaded ELF apps.
 */
#pragma once

/** Publish the syscall table to the ELF loader. Call once, before any app runs. */
void neos_syscalls_register(void);
