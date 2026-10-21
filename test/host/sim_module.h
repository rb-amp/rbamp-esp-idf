/* Host simulation of a BASIC module behind the IDF calls rbamp.c makes.
 * One 256-byte register image; writes land in it, reads come from it; the
 * channel window serves a float that encodes (field, ch); COMMAND writes go
 * to a test hook so a scenario can model what the firmware does. */
#pragma once
#include <stdint.h>
#include <stddef.h>

extern uint8_t  sim_regs[256];
extern unsigned sim_txn, sim_select_writes, sim_cmd_writes;
extern void   (*sim_on_command)(uint8_t opcode);   /* NULL = ignore */

void  sim_reset_image(void);                       /* zero image, counters, hook */
void  sim_set_f32(uint8_t reg, float v);
float sim_get_f32(uint8_t reg);
void  sim_set_u16(uint8_t reg, uint16_t v);
uint16_t sim_get_u16(uint8_t reg);
float sim_window_value(uint8_t field, uint8_t ch);
