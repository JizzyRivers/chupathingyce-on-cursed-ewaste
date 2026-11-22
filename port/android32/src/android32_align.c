/*
ANDROID32_ALIGN.C

A 32-bit program on a 64-bit Android kernel (the S905W2's, and most boxes with
a 32-bit system) gets SIGBUS (BUS_ADRALN) for the instructions that need an
aligned address: LDM, STM, LDRD, STRD and the VFP's VLDR, VSTR, VLDM, VSTM
(the NEON VLD1 and VST1 with an alignment hint). A plain LDR or LDRH may be
unaligned, but these may not, and the Xbox game's structures sit at odd
addresses wherever its data does (a render_window in an array whose elements
are not a multiple of 4 bytes, a map's animation data): the compiler copies
a structure of three floats with LDM and STM, as it must where it takes the
address to be aligned. On x86 nothing notices; the 32-bit Linux kernels the
Steam Link and Raspberry Pi run have an alignment trap that does what this
file does (/proc/cpu/alignment), a 64-bit kernel does not for 32-bit tasks.

So this is the trap, in the program: a SIGBUS handler that decodes the
faulting ARM-state instruction, does its loads and stores a byte at a time,
and goes on at the next instruction. Anything it does not know is left to
the default action, so the crash report is the system's. The count and the
first places are logged (ANDROID32_ALIGN_LOG), because each fixed access
costs a signal: the game's hottest sites are worth fixing in the source.
*/

#include <android/log.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#define LOG_TAG "chupathingyce"

/* the registers as an array: ucontext's arm_r0 .. arm_pc follow each other */
#define REGISTERS(context) (&(context)->uc_mcontext.arm_r0)
#define CPSR(context) ((context)->uc_mcontext.arm_cpsr)

static volatile unsigned long fixed_count;
static unsigned long logged;

struct vfp_state
{
	uint64_t *registers; /* d0 .. d31 */
};

/* the VFP registers of the signal frame (the kernel puts them back when the
handler returns): its extended frames follow uc_regspace */
static int vfp_registers(ucontext_t *context, struct vfp_state *state)
{
	unsigned long *frame = (unsigned long *)context->uc_regspace;
	unsigned long *end = frame + sizeof(context->uc_regspace) / sizeof(unsigned long);

	while (frame + 2 <= end && frame[0] != 0)
	{
		if (frame[0] == 0x56465001UL) /* VFP_MAGIC */
		{
			state->registers = (uint64_t *)(frame + 2);
			return 1;
		}
		if (frame[1] < 8)
			break;
		frame += frame[1] / sizeof(unsigned long);
	}
	return 0;
}

static uint32_t load32(uint32_t address)
{
	const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;

	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void store32(uint32_t address, uint32_t value)
{
	uint8_t *bytes = (uint8_t *)(uintptr_t)address;

	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static int popcount16(uint32_t value)
{
	int count = 0;

	for (; value; value &= value - 1)
		count++;
	return count;
}

/* the value of a register as an instruction reads it: the program counter is
the instruction's address and 8 */
static uint32_t read_register(const unsigned long *registers, unsigned index, uint32_t pc)
{
	return index == 15 ? pc + 8 : (uint32_t)registers[index];
}

/* LDM and STM, every addressing mode, with writeback */
static int emulate_block(unsigned long *registers, uint32_t instruction, uint32_t pc, int *branched)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, writeback = (instruction >> 21) & 1;
	int load = (instruction >> 20) & 1;
	unsigned base = (instruction >> 16) & 15;
	uint32_t list = instruction & 0xFFFF;
	int count = popcount16(list);
	uint32_t base_value = read_register(registers, base, pc), address;
	unsigned index;

	if (!count || (instruction >> 22) & 1 /* S: user registers or CPSR, never for the game */)
		return 0;
	if (up)
		address = pre ? base_value + 4 : base_value;
	else
		address = pre ? base_value - 4 * (uint32_t)count : base_value - 4 * (uint32_t)count + 4;
	for (index = 0; index < 16; index++)
	{
		if (!(list & (1u << index)))
			continue;
		if (load)
		{
			uint32_t value = load32(address);

			if (index == 15)
			{
				registers[15] = value & ~3u;
				*branched = 1;
			}
			else
				registers[index] = value;
		}
		else
			store32(address, index == 15 ? pc + 8 : (uint32_t)registers[index]);
		address += 4;
	}
	if (writeback && !(load && (list & (1u << base))))
		registers[base] = up ? base_value + 4 * (uint32_t)count : base_value - 4 * (uint32_t)count;
	return 1;
}

/* LDRD and STRD */
static int emulate_double(unsigned long *registers, uint32_t instruction, uint32_t pc)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, immediate = (instruction >> 22) & 1;
	int writeback = (instruction >> 21) & 1, store = ((instruction >> 5) & 3) == 3;
	unsigned base = (instruction >> 16) & 15, target = (instruction >> 12) & 15;
	uint32_t offset = immediate ? (((instruction >> 4) & 0xF0) | (instruction & 0xF))
	                            : read_register(registers, instruction & 15, pc);
	uint32_t base_value = read_register(registers, base, pc);
	uint32_t offset_address = up ? base_value + offset : base_value - offset;
	uint32_t address = pre ? offset_address : base_value;

	if (target & 1 || target == 14)
		return 0;
	if (store)
	{
		store32(address, (uint32_t)registers[target]);
		store32(address + 4, (uint32_t)registers[target + 1]);
	}
	else
	{
		registers[target] = load32(address);
		registers[target + 1] = load32(address + 4);
	}
	if (!pre || writeback)
		registers[base] = offset_address;
	return 1;
}

/* the VFP's VLDR, VSTR, VLDM and VSTM */
static int emulate_vfp(ucontext_t *context, unsigned long *registers, uint32_t instruction, uint32_t pc)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, writeback = (instruction >> 21) & 1;
	int load = (instruction >> 20) & 1, doubles = (instruction >> 8) & 1;
	unsigned base = (instruction >> 16) & 15, immediate = instruction & 0xFF;
	unsigned first = doubles ? (((instruction >> 22) & 1) << 4) | ((instruction >> 12) & 15)
	                         : (((instruction >> 12) & 15) << 1) | ((instruction >> 22) & 1);
	uint32_t base_value = base == 15 ? (pc + 8) & ~3u : (uint32_t)registers[base];
	uint32_t address, words, word;
	struct vfp_state vfp;

	if (!vfp_registers(context, &vfp))
		return 0;
	if (pre && !writeback) /* VLDR, VSTR: one register at base +/- imm8 * 4 */
	{
		address = up ? base_value + immediate * 4 : base_value - immediate * 4;
		words = doubles ? 2 : 1;
	}
	else /* VLDM, VSTM (VPOP, VPUSH): imm8 words from the lowest address */
	{
		words = immediate;
		if (!words || (doubles && (words & 1)) || (!doubles && first + words > 32) || (doubles && first + words / 2 > 32))
			return 0;
		address = up ? base_value : base_value - words * 4;
		if (!pre && !up)
			return 0; /* (decrement after is not a VFP mode) */
		if (writeback)
			registers[base] = up ? base_value + words * 4 : base_value - words * 4;
	}
	for (word = 0; word < words; word++, address += 4)
	{
		/* word 2k of a double register is its low half */
		unsigned register_index = doubles ? first + word / 2 : (first + word) / 2;
		int high = doubles ? (int)(word & 1) : (int)((first + word) & 1);
		uint64_t *d = &vfp.registers[register_index];
		uint32_t *half = (uint32_t *)d + high;

		if (register_index > 31)
			return 0;
		if (load)
			*half = load32(address);
		else
			store32(address, *half);
	}
	return 1;
}

static void alignment_fault(int signal_number, siginfo_t *information, void *argument)
{
	ucontext_t *context = (ucontext_t *)argument;
	unsigned long *registers = REGISTERS(context);
	uint32_t pc = (uint32_t)registers[15], instruction;
	int handled = 0, branched = 0;

	if (information->si_code == BUS_ADRALN && !(CPSR(context) & 0x20) /* ARM state */)
	{
		instruction = *(const uint32_t *)(uintptr_t)pc;
		if (((instruction >> 25) & 7) == 4)
			handled = emulate_block(registers, instruction, pc, &branched);
		else if (((instruction >> 25) & 7) == 0 && (instruction & 0x90) == 0x90 && ((instruction >> 5) & 3) >= 2 &&
			!((instruction >> 20) & 1))
			handled = emulate_double(registers, instruction, pc);
		else if (((instruction >> 25) & 7) == 6 && ((instruction >> 9) & 7) == 5)
			handled = emulate_vfp(context, registers, instruction, pc);
		if (handled)
		{
			unsigned long count = ++fixed_count;

			if (!branched)
				registers[15] = pc + 4;
			if (logged < 40 && (count <= 20 || count % 100000 == 0))
			{
				logged++;
				__android_log_print(ANDROID_LOG_INFO, LOG_TAG,
					"alignment fixup #%lu: %08x at pc %08x for address %08lx", count, instruction, pc,
					(unsigned long)(uintptr_t)information->si_addr);
			}
			return;
		}
		__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "alignment fault not fixed: %08x at pc %08x for address %p",
			instruction, pc, information->si_addr);
	}
	/* anything else: the system's crash report */
	signal(signal_number, SIG_DFL);
}

void android32_alignment_fixups(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = alignment_fault;
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	sigaction(SIGBUS, &action, NULL);
}

unsigned long android32_alignment_fixup_count(void)
{
	return fixed_count;
}
