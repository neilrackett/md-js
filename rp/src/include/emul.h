/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the MD/JS runtime bootstrap.
 */

#ifndef EMUL_H
#define EMUL_H

/**
 * @brief Launch the MD/JS runtime.
 *
 * Copies the ST-side cartridge binary into ROM-in-RAM, initializes ROM
 * emulation and command parsing, starts the Core 1 JerryScript worker, and
 * enters the main dispatch loop.
 */
void emul_start();

#endif  // EMUL_H
