
#define pr_fmt(fmt) "alternatives: " fmt

#include <dim-sum/init.h>
#include <dim-sum/cpu.h>
#include <dim-sum/string.h>
#include <asm/cacheflush.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>

extern struct alt_instr __alt_instructions[], __alt_instructions_end[];

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

static int __apply_alternatives(void *alt_region)
{
	struct alt_instr *alt;
	struct alt_region *region = alt_region;
	u8 *origptr, *replptr;

	for (alt = region->begin; alt < region->end; alt++) {
		if (!cpus_have_cap(alt->cpufeature))
			continue;

		BUG_ON(alt->alt_len != alt->orig_len);

		//pr_info_once("patching kernel code\n");

		origptr = (u8 *)&alt->orig_offset + alt->orig_offset;
		replptr = (u8 *)&alt->alt_offset + alt->alt_offset;
		memcpy(origptr, replptr, alt->alt_len);
		flush_icache_range((uintptr_t)origptr,
				   (uintptr_t)(origptr + alt->alt_len));
	}

	return 0;
}

void apply_alternatives_all(void)
{
	struct alt_region region = {
		.begin	= __alt_instructions,
		.end	= __alt_instructions_end,
	};

	__apply_alternatives(&region);
}

void apply_alternatives(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};

	__apply_alternatives(&region);
}
